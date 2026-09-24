package com.hikaboshi.companion.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import java.util.UUID
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

data class DiscoveredDevice(
    val address: String,
    val name: String?,
    val rssi: Int,
)

data class WatchState(
    val connected: Boolean = false,
    val connecting: Boolean = false,
    val address: String? = null,
    val battery: Int? = null,
    val steps: Long? = null,
    val lastNotification: Protocol.WatchNotification? = null,
    val lastControlMessage: String? = null,
    val lastOtaStatus: String? = null,
    val lastOtaProgress: Int? = null,
    val log: List<String> = emptyList(),
    val scanResults: List<DiscoveredDevice> = emptyList(),
    val scanning: Boolean = false,
    val error: String? = null,
)

/**
 * Central BLE manager for Hikaboshi.
 * Callbacks hop onto the main thread for StateFlow safety.
 */
class WatchBleManager(private val context: Context) {

    companion object {
        private const val TAG = "WatchBle"
        private const val CCCD = "00002902-0000-1000-8000-00805f9b34fb"
        private const val MAX_LOG = 200
    }

    private val _state = MutableStateFlow(WatchState())
    val state: StateFlow<WatchState> = _state.asStateFlow()

    /** External hooks (find-phone ring, music keys). */
    var onControlEvent: ((String) -> Unit)? = null

    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter get() = bluetoothManager.adapter
    private val scanner get() = adapter.bluetoothLeScanner

    private var gatt: BluetoothGatt? = null
    private var notifyChar: BluetoothGattCharacteristic? = null
    private var controlChar: BluetoothGattCharacteristic? = null
    private var batteryChar: BluetoothGattCharacteristic? = null
    private var stepsChar: BluetoothGattCharacteristic? = null

    private var pendingConnect: ((Result<BluetoothGatt>) -> Unit)? = null
    private var pendingWrite: ((Result<Unit>) -> Unit)? = null

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: result.scanRecord?.deviceName
            if (name != Protocol.DEVICE_NAME &&
                result.scanRecord?.serviceUuids?.none { it.uuid == Protocol.SERVICE } == true
            ) {
                return
            }
            val dev = DiscoveredDevice(result.device.address, name, result.rssi)
            val current = _state.value.scanResults
            val idx = current.indexOfFirst { it.address == dev.address }
            val next = if (idx >= 0) current.toMutableList().also { it[idx] = dev } else current + dev
            _state.value = _state.value.copy(scanResults = next.sortedByDescending { it.rssi })
        }

        override fun onScanFailed(errorCode: Int) {
            appendLog("Scan failed: $errorCode")
            _state.value = _state.value.copy(scanning = false, error = "Scan failed ($errorCode)")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (status != BluetoothGatt.GATT_SUCCESS) {
                        appendLog("Connect status error=$status")
                    }
                    appendLog("Connected (status=$status)")
                    discoverAttempt = 0
                    // Discover immediately, same as it works from other BLE clients
                    // (e.g. generic scanner apps) that don't touch the cache up front.
                    // Calling gatt.refresh() (hidden API) here and only then discovering
                    // was itself the bug: refresh() is async with no completion signal,
                    // so discoverServices() 300ms later could run before the stack
                    // rebuilt its attribute table, yielding GATT_SUCCESS + 0 services
                    // every time. refresh() is now only used in retryDiscovery(), where
                    // an actually-stale cache is the plausible explanation.
                    appendLog("discoverServices()…")
                    gatt.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    appendLog("Disconnected (status=$status)")
                    val pend = pendingConnect
                    pendingConnect = null
                    pend?.invoke(Result.failure(IllegalStateException("Disconnected")))
                    gatt.close()
                    if (this@WatchBleManager.gatt === gatt) {
                        this@WatchBleManager.gatt = null
                    }
                    _state.value = _state.value.copy(
                        connected = false,
                        connecting = false,
                        battery = null,
                        steps = null,
                    )
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                if (retryDiscovery(gatt, "status=$status")) return
                failDiscovery(gatt, "Service discovery failed: $status")
                return
            }

            var service = gatt.getService(Protocol.SERVICE)
            if (service == null) {
                // Fallback: locate by characteristic UUID (handles rare UUID presentation quirks).
                service = gatt.services.firstOrNull { svc ->
                    svc.getCharacteristic(Protocol.CHAR_CONTROL) != null ||
                        svc.getCharacteristic(Protocol.CHAR_NOTIFICATION) != null
                }
            }

            if (service == null) {
                val found = gatt.services.joinToString { it.uuid.toString() }
                appendLog("Discovery#${discoverAttempt + 1}: ${gatt.services.size} services [$found]")
                if (retryDiscovery(gatt, "service missing (seen: $found)")) return
                failDiscovery(gatt, "Hikaboshi service not found (device has: $found)")
                return
            }

            appendLog("Found service ${service.uuid} (${service.characteristics.size} chars)")

            notifyChar = service.getCharacteristic(Protocol.CHAR_NOTIFICATION)
            controlChar = service.getCharacteristic(Protocol.CHAR_CONTROL)
            batteryChar = service.getCharacteristic(Protocol.CHAR_BATTERY)
            stepsChar = service.getCharacteristic(Protocol.CHAR_STEPS)

            if (controlChar == null || notifyChar == null) {
                if (retryDiscovery(gatt, "chars missing")) return
                failDiscovery(gatt, "Required characteristics missing")
                return
            }

            // Enable notifications on all four, then resolve connect.
            val chars = listOfNotNull(notifyChar, controlChar, batteryChar, stepsChar)
            enableNotifications(gatt, chars, 0) { ok ->
                if (ok) {
                    _state.value = _state.value.copy(
                        connected = true,
                        connecting = false,
                        address = gatt.device.address,
                        error = null,
                    )
                    appendLog("Ready (${gatt.device.address})")
                    pendingConnect?.let { it(Result.success(gatt)); pendingConnect = null }
                    // Prime reads
                    batteryChar?.let { gatt.readCharacteristic(it) }
                } else {
                    val e = IllegalStateException("Failed to enable notifications")
                    pendingConnect?.let { it(Result.failure(e)); pendingConnect = null }
                    _state.value = _state.value.copy(connecting = false, error = e.message)
                }
            }
        }

        private fun retryDiscovery(gatt: BluetoothGatt, why: String): Boolean {
            discoverAttempt++
            if (discoverAttempt > 3) {
                appendLog("Discovery gave up: $why")
                return false
            }
            appendLog("Discovery retry $discoverAttempt/3 ($why)")
            android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                if (this@WatchBleManager.gatt === gatt) {
                    refreshGattCache(gatt)
                    gatt.discoverServices()
                }
            }, 500L * discoverAttempt)
            return true
        }

        private fun failDiscovery(gatt: BluetoothGatt, message: String) {
            // Close so the watch (max 1 conn) can accept the next attempt.
            try {
                gatt.disconnect()
                gatt.close()
            } catch (_: Exception) {
            }
            if (this@WatchBleManager.gatt === gatt) {
                this@WatchBleManager.gatt = null
            }
            val e = IllegalStateException(message)
            pendingConnect?.let { it(Result.failure(e)); pendingConnect = null }
            _state.value = _state.value.copy(
                connecting = false,
                connected = false,
                error = e.message,
            )
        }

        private fun enableNotifications(
            gatt: BluetoothGatt,
            chars: List<BluetoothGattCharacteristic>,
            index: Int,
            done: (Boolean) -> Unit,
        ) {
            if (index >= chars.size) {
                done(true)
                return
            }
            val c = chars[index]
            gatt.setCharacteristicNotification(c, true)
            val cccd = c.getDescriptor(UUID.fromString(CCCD))
            if (cccd == null) {
                enableNotifications(gatt, chars, index + 1, done)
                return
            }
            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            val started = gatt.writeDescriptor(cccd)
            if (!started) {
                done(false)
                return
            }
            // Continuation is handled in onDescriptorWrite via a small queue.
            pendingDescWrite = {
                enableNotifications(gatt, chars, index + 1, done)
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                appendLog("CCCD write failed: $status")
            }
            val next = pendingDescWrite
            pendingDescWrite = null
            next?.invoke()
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleChanged(characteristic.uuid, value)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            @Suppress("DEPRECATION")
            handleChanged(characteristic.uuid, characteristic.value ?: ByteArray(0))
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS) handleChanged(characteristic.uuid, value)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) return
            @Suppress("DEPRECATION")
            handleChanged(characteristic.uuid, characteristic.value ?: ByteArray(0))
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            val pend = pendingWrite
            pendingWrite = null
            if (status == BluetoothGatt.GATT_SUCCESS) {
                pend?.invoke(Result.success(Unit))
            } else {
                pend?.invoke(Result.failure(IllegalStateException("Write failed: $status")))
            }
        }
    }

    private var pendingDescWrite: (() -> Unit)? = null
    private var discoverAttempt = 0

    private fun handleChanged(uuid: UUID, value: ByteArray) {
        when (uuid) {
            Protocol.CHAR_BATTERY -> {
                val pct = Protocol.parseBattery(value)
                if (pct != null) _state.value = _state.value.copy(battery = pct)
            }
            Protocol.CHAR_STEPS -> {
                val steps = Protocol.parseSteps(value)
                if (steps != null) _state.value = _state.value.copy(steps = steps)
            }
            Protocol.CHAR_NOTIFICATION -> {
                // Could be a write-ACK (echo) or a real notify; try parse.
                Protocol.parseNotification(value)?.let { n ->
                    if (n.hasData) {
                        _state.value = _state.value.copy(lastNotification = n)
                    }
                }
            }
            Protocol.CHAR_CONTROL -> {
                val msg = value.toString(Charsets.UTF_8)
                if (msg.isNotEmpty()) {
                    _state.value = _state.value.copy(lastControlMessage = msg)
                    when {
                        msg.startsWith("ota_status=") ->
                            _state.value = _state.value.copy(lastOtaStatus = msg.removePrefix("ota_status="))
                        msg.startsWith("ota_progress=") ->
                            _state.value = _state.value.copy(
                                lastOtaProgress = msg.removePrefix("ota_progress=").toIntOrNull()
                            )
                    }
                    // Forward find-phone / music events (ignore pure write-echo ACKs of our own cmds)
                    if (msg in listOf(
                            Protocol.EVT_FIND_PHONE,
                            Protocol.EVT_FIND_PHONE_STOP,
                            Protocol.EVT_MUSIC_TOGGLE,
                            Protocol.EVT_MUSIC_NEXT,
                            Protocol.EVT_MUSIC_PREV,
                        )
                    ) {
                        onControlEvent?.invoke(msg)
                    }
                    appendLog("← $msg")
                }
            }
        }
    }


    @SuppressLint("MissingPermission")
    private suspend fun clearStaleBond(device: BluetoothDevice) {
        if (device.bondState != BluetoothDevice.BOND_BONDED) return
        appendLog("Stale bond found for ${device.address}, removing…")
        val cleared = withTimeoutOrNull(2000L) {
            suspendCancellableCoroutine<Unit> { cont ->
                val receiver = object : BroadcastReceiver() {
                    override fun onReceive(ctx: Context, intent: Intent) {
                        val changed = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                        if (changed?.address != device.address) return
                        val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
                        if (state == BluetoothDevice.BOND_NONE) {
                            try {
                                context.unregisterReceiver(this)
                            } catch (_: Exception) {
                            }
                            if (cont.isActive) cont.resume(Unit)
                        }
                    }
                }
                cont.invokeOnCancellation {
                    try {
                        context.unregisterReceiver(receiver)
                    } catch (_: Exception) {
                    }
                }
                context.registerReceiver(receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
                val removed = try {
                    val m = BluetoothDevice::class.java.getMethod("removeBond")
                    m.invoke(device) as Boolean
                } catch (e: Exception) {
                    appendLog("removeBond() unavailable: ${e.message}")
                    false
                }
                if (!removed) {
                    try {
                        context.unregisterReceiver(receiver)
                    } catch (_: Exception) {
                    }
                    if (cont.isActive) cont.resume(Unit)
                }
            }
        }
        appendLog(if (cleared != null) "Bond cleared" else "Bond clear timed out, continuing anyway")
    }

    private fun refreshGattCache(gatt: BluetoothGatt): Boolean {
        return try {
            val m = BluetoothGatt::class.java.getMethod("refresh")
            m.invoke(gatt) as Boolean
        } catch (e: Exception) {
            appendLog("refresh() unavailable: ${e.message}")
            false
        }
    }

    private fun appendLog(line: String) {
        val next = (_state.value.log + line).takeLast(MAX_LOG)
        _state.value = _state.value.copy(log = next)
        Log.d(TAG, line)
    }

    private fun hasBlePermission(): Boolean {
        val scan = ContextCompat.checkSelfPermission(
            context, android.Manifest.permission.BLUETOOTH_SCAN
        ) == PackageManager.PERMISSION_GRANTED
        val connect = ContextCompat.checkSelfPermission(
            context, android.Manifest.permission.BLUETOOTH_CONNECT
        ) == PackageManager.PERMISSION_GRANTED
        return scan && connect
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (!hasBlePermission()) {
            _state.value = _state.value.copy(error = "Bluetooth permission missing")
            return
        }
        if (adapter?.isEnabled != true) {
            _state.value = _state.value.copy(error = "Bluetooth is off")
            return
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        _state.value = _state.value.copy(scanResults = emptyList(), scanning = true, error = null)
        scanner?.startScan(null, settings, scanCallback)
        appendLog("Scanning…")
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (!hasBlePermission()) return
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        _state.value = _state.value.copy(scanning = false)
    }

    @SuppressLint("MissingPermission")
    suspend fun connect(address: String) {
        if (!hasBlePermission()) {
            _state.value = _state.value.copy(error = "Bluetooth permission missing")
            return
        }
        // Already connected to this device — don't tear down and reconnect.
        val existing = gatt
        if (existing != null &&
            _state.value.connected &&
            existing.device.address.equals(address, ignoreCase = true)
        ) {
            appendLog("Already connected to $address")
            return
        }
        stopScan()
        disconnect()
        _state.value = _state.value.copy(connecting = true, error = null)
        appendLog("Connecting to $address…")

        val device: BluetoothDevice = adapter!!.getRemoteDevice(address)
        // The firmware doesn't require bonding, but a stale bond left over from
        // earlier testing (when bonding was still enabled) makes the OS reuse a
        // dead LTK/GATT cache: onServicesDiscovered fires with GATT_SUCCESS but
        // an empty service list, on every attempt. Clear it before connecting.
        clearStaleBond(device)

        suspendCancellableCoroutine { cont ->
            pendingConnect = { result ->
                if (cont.isActive) {
                    result.fold(
                        onSuccess = { cont.resume(Unit) },
                        onFailure = { e ->
                            _state.value = _state.value.copy(error = e.message)
                            cont.resumeWithException(e)
                        },
                    )
                }
            }
            cont.invokeOnCancellation { pendingConnect = null }
            // Without an explicit transport, some stacks (seen on MIUI/HyperOS)
            // pick a transport that connects fine at the link layer but never
            // actually surfaces GATT attributes to this app: onServicesDiscovered
            // keeps firing GATT_SUCCESS with an empty list no matter how long you
            // wait or how many times you retry/refresh, while other BLE apps on
            // the same phone/device connect and discover normally. Forcing LE
            // fixes that class of failure.
            gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (_: Exception) {
        }
        gatt = null
        notifyChar = null
        controlChar = null
        batteryChar = null
        stepsChar = null
        _state.value = _state.value.copy(connected = false, connecting = false)
    }

    @SuppressLint("MissingPermission")
    private suspend fun write(uuid: UUID, payload: ByteArray) {
        val g = gatt ?: throw IllegalStateException("Not connected")
        val c = when (uuid) {
            Protocol.CHAR_NOTIFICATION -> notifyChar
            Protocol.CHAR_CONTROL -> controlChar
            else -> g.getService(Protocol.SERVICE)?.getCharacteristic(uuid)
        } ?: throw IllegalStateException("Characteristic missing")
        c.value = payload
        suspendCancellableCoroutine { cont ->
            pendingWrite = { result ->
                if (cont.isActive) {
                    result.fold(
                        onSuccess = { cont.resume(Unit) },
                        onFailure = { cont.resumeWithException(it) },
                    )
                }
            }
            cont.invokeOnCancellation { pendingWrite = null }
            if (!g.writeCharacteristic(c)) {
                pendingWrite = null
                cont.resumeWithException(IllegalStateException("writeCharacteristic rejected"))
            }
        }
        appendLog("→ ${payload.toString(Charsets.UTF_8).ifEmpty { payload.joinToString() }}")
    }

    suspend fun sendNotification(title: String, body: String) {
        write(Protocol.CHAR_NOTIFICATION, Protocol.notificationPayload(title, body))
    }

    suspend fun sendCommand(command: String) {
        write(Protocol.CHAR_CONTROL, command.toByteArray(Charsets.UTF_8))
    }

    suspend fun sendTimeSync() = sendCommand(Protocol.cmdTime(System.currentTimeMillis() / 1000))
    suspend fun sendWeather(temp: Double, code: Int) = sendCommand(Protocol.cmdWeather(temp, code))
    suspend fun sendOta(url: String) = sendCommand(Protocol.cmdOta(url))
    suspend fun sendScreen(on: Boolean) = sendCommand(Protocol.cmdScreen(on))
    suspend fun sendWifi(on: Boolean) = sendCommand(Protocol.cmdWifi(on))

    fun refreshReads() {
        val g = gatt ?: return
        batteryChar?.let { g.readCharacteristic(it) }
        stepsChar?.let { g.readCharacteristic(it) }
    }
}
