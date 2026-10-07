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
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
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
    val reconnecting: Boolean = false,
    val address: String? = null,
    val lastAddress: String? = null,
    val autoConnect: Boolean = true,
    val autoWeather: Boolean = false,
    val battery: Int? = null,
    val steps: Long? = null,
    val distanceM: Long? = null,
    val caloriesKcal: Double? = null,
    val mtu: Int? = null,
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

        /* A rejected write means the GATT TX queue was momentarily busy. */
        private const val WRITE_ATTEMPTS = 4
        private const val WRITE_RETRY_MS = 60L
        private const val WRITE_CALLBACK_TIMEOUT_MS = 3_000L
        private val RECONNECT_DELAYS = longArrayOf(5, 10, 20, 30, 60, 60, 60, 60)
    }

    private val _state = MutableStateFlow(WatchState())
    val state: StateFlow<WatchState> = _state.asStateFlow()

    /**
     * Watch -> phone control events (find-phone, music keys, dismiss, DND toggle).
     * A SharedFlow so the service, the ViewModel, and the notification listener
     * can each collect independently without clobbering one another's handler.
     */
    private val _controlEvents = MutableSharedFlow<String>(extraBufferCapacity = 8)
    val controlEvents: SharedFlow<String> = _controlEvents.asSharedFlow()

    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter get() = bluetoothManager.adapter
    private val scanner get() = adapter.bluetoothLeScanner

    private var gatt: BluetoothGatt? = null
    private var notifyChar: BluetoothGattCharacteristic? = null
    private var controlChar: BluetoothGattCharacteristic? = null
    private var batteryChar: BluetoothGattCharacteristic? = null
    private var stepsChar: BluetoothGattCharacteristic? = null
    private var distanceChar: BluetoothGattCharacteristic? = null
    private var caloriesChar: BluetoothGattCharacteristic? = null
    private var mediaChar: BluetoothGattCharacteristic? = null

    private var pendingConnect: ((Result<BluetoothGatt>) -> Unit)? = null

    /* Only one GATT write may be in flight at a time, and Android reports a
     * second concurrent attempt as writeCharacteristic()==false ("rejected").
     * Several coroutines write independently (media poll, time-sync,
     * notifications, weather), so writes are serialised through this mutex
     * rather than sharing a single pendingWrite slot - concurrent callers would
     * otherwise clobber each other's callback and one would hang forever. */
    private val writeMutex = Mutex()
    private var pendingWrite: ((Result<Unit>) -> Unit)? = null

    private val prefs = context.getSharedPreferences(BleHolder.PREFS, Context.MODE_PRIVATE)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var reconnectJob: Job? = null
    private var userDisconnected = false

    init {
        _state.value = _state.value.copy(
            lastAddress = prefs.getString(BleHolder.KEY_ADDRESS, null),
            autoConnect = prefs.getBoolean(BleHolder.KEY_AUTO_CONNECT, true),
            autoWeather = prefs.getBoolean(BleHolder.KEY_AUTO_WEATHER, false),
        )
    }

    /** Persisted toggle for auto-connect + background reconnect. */
    @SuppressLint("MissingPermission")
    fun setAutoConnect(enabled: Boolean) {
        prefs.edit().putBoolean(BleHolder.KEY_AUTO_CONNECT, enabled).apply()
        _state.value = _state.value.copy(autoConnect = enabled)
        if (!enabled) {
            reconnectJob?.cancel()
            reconnectJob = null
            // A passive (autoConnect=true) GATT client can be waiting indefinitely
            // for the watch to reappear; close it so it stops registering with
            // the Bluetooth stack. Never touch a live connection here.
            if (!_state.value.connected) {
                try {
                    gatt?.close()
                } catch (_: Exception) {
                }
                gatt = null
            }
            _state.value = _state.value.copy(reconnecting = false)
        }
        appendLog("Auto-connect ${if (enabled) "on" else "off"}")
    }

    /** Forget the remembered device (also disconnects). */
    fun forgetDevice() {
        prefs.edit().remove(BleHolder.KEY_ADDRESS).apply()
        _state.value = _state.value.copy(lastAddress = null)
        disconnect()
        appendLog("Forgot device")
    }

    /** Read by the notification listener; kept here so all prefs live together. */
    fun forwardingEnabled(): Boolean =
        prefs.getBoolean(BleHolder.KEY_FORWARD, false)

    fun setForwarding(enabled: Boolean) {
        prefs.edit().putBoolean(BleHolder.KEY_FORWARD, enabled).apply()
        appendLog("Notification forwarding ${if (enabled) "on" else "off"}")
    }

    /** Persisted toggle: fetch device-location weather and push it on every connect. */
    fun setAutoWeather(enabled: Boolean) {
        prefs.edit().putBoolean(BleHolder.KEY_AUTO_WEATHER, enabled).apply()
        _state.value = _state.value.copy(autoWeather = enabled)
        appendLog("Auto-weather ${if (enabled) "on" else "off"}")
    }

    /** Fetch device-location weather and push it now, regardless of the auto-weather toggle. */
    suspend fun refreshWeatherNow(): Boolean {
        val result = WeatherFetcher.fetchForDeviceLocation(context) ?: run {
            appendLog("Weather refresh failed (no location/network)")
            return false
        }
        sendWeather(result.tempC, result.wmoCode)
        appendLog("Weather refreshed: ${result.tempC}°C code ${result.wmoCode}")
        return true
    }

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
                    val wasConnected = _state.value.connected
                    _state.value = _state.value.copy(
                        connected = false,
                        connecting = false,
                        battery = null,
                        steps = null,
                        distanceM = null,
                        caloriesKcal = null,
                        mtu = null,
                    )
                    // Unexpected drop (not a manual disconnect): back off and retry.
                    if (!userDisconnected && (wasConnected || _state.value.reconnecting)) {
                        scheduleReconnect()
                    } else {
                        _state.value = _state.value.copy(reconnecting = false)
                    }
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
            distanceChar = service.getCharacteristic(Protocol.CHAR_DISTANCE)
            caloriesChar = service.getCharacteristic(Protocol.CHAR_CALORIES)
            mediaChar = service.getCharacteristic(Protocol.CHAR_MEDIA)

            if (controlChar == null || notifyChar == null) {
                if (retryDiscovery(gatt, "chars missing")) return
                failDiscovery(gatt, "Required characteristics missing")
                return
            }

            // Media is optional (older firmware won't have it), but its absence
            // after a cache refresh usually means Android served a stale GATT
            // table from before the characteristic was added.
            if (mediaChar == null) {
                appendLog("Media char absent (${service.characteristics.size} chars seen)")
            }

            // Enable notifications on all known chars, then resolve connect.
            val chars = listOfNotNull(
                notifyChar, controlChar, batteryChar, stepsChar,
                distanceChar, caloriesChar,
            )
            enableNotifications(gatt, chars, 0) { ok ->
                if (ok) {
                    _state.value = _state.value.copy(
                        connected = true,
                        connecting = false,
                        reconnecting = false,
                        address = gatt.device.address,
                        lastAddress = gatt.device.address,
                        error = null,
                    )
                    prefs.edit().putString(BleHolder.KEY_ADDRESS, gatt.device.address).apply()
                    appendLog("Ready (${gatt.device.address})")
                    pendingConnect?.let { it(Result.success(gatt)); pendingConnect = null }
                    // Prime reads (fitness + battery)
                    batteryChar?.let { gatt.readCharacteristic(it) }
                    stepsChar?.let { gatt.readCharacteristic(it) }
                    distanceChar?.let { gatt.readCharacteristic(it) }
                    caloriesChar?.let { gatt.readCharacteristic(it) }
                    // The watch has no battery-backed clock guarantee; sync on every
                    // connect so alarms and timestamps are right.
                    this@WatchBleManager.scope.launch {
                        try {
                            sendTimeSync()
                            appendLog("Auto time-sync sent")
                        } catch (e: Exception) {
                            appendLog("Auto time-sync failed: ${e.message}")
                        }
                    }
                    if (_state.value.autoWeather) {
                        this@WatchBleManager.scope.launch {
                            try {
                                refreshWeatherNow()
                            } catch (e: Exception) {
                                appendLog("Auto-weather failed: ${e.message}")
                            }
                        }
                    }
                    // Notification payloads can be 161 B; default MTU (23) truncates
                    // reads. Request the firmware's preferred 256.
                    try {
                        gatt.requestMtu(256)
                    } catch (e: Exception) {
                        appendLog("requestMtu failed: ${e.message}")
                    }
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

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                negotiatedMtu = mtu
                appendLog("MTU negotiated: $mtu")
                _state.value = _state.value.copy(mtu = mtu)
            } else {
                appendLog("MTU request failed: $status")
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
            Protocol.CHAR_DISTANCE -> {
                val m = Protocol.parseDistanceM(value)
                if (m != null) _state.value = _state.value.copy(distanceM = m)
            }
            Protocol.CHAR_CALORIES -> {
                val kcal = Protocol.parseCaloriesKcal(value)
                if (kcal != null) _state.value = _state.value.copy(caloriesKcal = kcal)
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
                    // Forward watch-initiated events (ignore pure write-echo ACKs of our own cmds)
                    if (msg in listOf(
                            Protocol.EVT_FIND_PHONE,
                            Protocol.EVT_FIND_PHONE_STOP,
                            Protocol.EVT_MUSIC_TOGGLE,
                            Protocol.EVT_MUSIC_NEXT,
                            Protocol.EVT_MUSIC_PREV,
                            Protocol.EVT_ALARM,
                            Protocol.EVT_DISMISS_NOTIF,
                            Protocol.EVT_DND_TOGGLE,
                        )
                    ) {
                        _controlEvents.tryEmit(msg)
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
        // Manual entry: stop any background reconnect loop first.
        reconnectJob?.cancel()
        reconnectJob = null
        _state.value = _state.value.copy(reconnecting = false)
        connectInternal(address)
    }

    /**
     * Boot/service entry: connect to the remembered device when auto-connect
     * is on. A failure schedules the background retry loop instead of
     * throwing all the way out.
     */
    suspend fun autoConnectIfRemembered(): Boolean {
        val addr = prefs.getString(BleHolder.KEY_ADDRESS, null) ?: return false
        if (!_state.value.autoConnect) return false
        if (_state.value.connected || _state.value.connecting) return true
        return try {
            connectInternal(addr)
            true
        } catch (e: Exception) {
            appendLog("Auto-connect failed (${e.message}), retrying in background")
            scheduleReconnect()
            false
        }
    }

    @SuppressLint("MissingPermission")
    private suspend fun connectInternal(address: String) {
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
        disconnectInternal(userInitiated = false)
        userDisconnected = false
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
        disconnectInternal(userInitiated = true)
    }

    @SuppressLint("MissingPermission")
    private fun disconnectInternal(userInitiated: Boolean) {
        if (userInitiated) {
            userDisconnected = true
            reconnectJob?.cancel()
            reconnectJob = null
        }
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
        distanceChar = null
        caloriesChar = null
        _state.value = _state.value.copy(
            connected = false,
            connecting = false,
            reconnecting = if (userInitiated) false else _state.value.reconnecting,
        )
    }

    /** Background retry loop with backoff; cancelled by manual connect/disconnect. */
    private fun scheduleReconnect() {
        val addr = _state.value.lastAddress
            ?: prefs.getString(BleHolder.KEY_ADDRESS, null)
            ?: return
        if (!_state.value.autoConnect) {
            _state.value = _state.value.copy(reconnecting = false)
            return
        }
        if (reconnectJob?.isActive == true) return
        reconnectJob = scope.launch {
            _state.value = _state.value.copy(reconnecting = true, error = null)
            for ((i, delayS) in RECONNECT_DELAYS.withIndex()) {
                if (_state.value.connected || userDisconnected) return@launch
                appendLog("Reconnect in ${delayS}s (attempt ${i + 1}/${RECONNECT_DELAYS.size})…")
                delay(delayS * 1000)
                if (_state.value.connected || userDisconnected) return@launch
                appendLog("Reconnect attempt ${i + 1}…")
                try {
                    connectInternal(addr)
                    return@launch
                } catch (e: Exception) {
                    if (e is kotlinx.coroutines.CancellationException) throw e
                    appendLog("Reconnect failed: ${e.message}")
                }
            }
            // Active attempts only succeed while the watch happens to be
            // advertising (it stops after ~5 min idle). Hand off to the
            // Bluetooth stack's passive auto-connect instead of polling with
            // active connects forever: near-zero radio/battery cost, and it
            // fires the moment the watch starts advertising again (e.g. a
            // wrist raise) rather than waiting for the next 60s tick.
            if (!_state.value.connected && !userDisconnected) {
                startPassiveReconnect(addr)
            } else {
                _state.value = _state.value.copy(reconnecting = false)
            }
        }
    }

    /** Registers a standing autoConnect=true GATT client; fires whenever the watch reappears. */
    @SuppressLint("MissingPermission")
    private fun startPassiveReconnect(address: String) {
        if (!hasBlePermission() || adapter?.isEnabled != true) {
            _state.value = _state.value.copy(reconnecting = false)
            return
        }
        appendLog("Active retries exhausted; waiting for watch to reappear…")
        stopScan()
        try {
            gatt?.close()
        } catch (_: Exception) {
        }
        val device = adapter!!.getRemoteDevice(address)
        gatt = device.connectGatt(context, true, gattCallback, BluetoothDevice.TRANSPORT_LE)
        // _state.reconnecting stays true; onConnectionStateChange(CONNECTED) or a
        // manual disconnect/forgetDevice/setAutoConnect(false) is what ends this wait.
    }

    @SuppressLint("MissingPermission")
    private var negotiatedMtu = 23

    private suspend fun write(uuid: UUID, payload: ByteArray) {
        // Serialise: one write in flight at a time, retries on a busy stack.
        writeMutex.withLock { writeLocked(uuid, payload) }
    }

    private suspend fun writeLocked(uuid: UUID, payload: ByteArray) {
        val g = gatt ?: throw IllegalStateException("Not connected")
        // A write longer than MTU-3 is silently dropped by the stack, so catch
        // it here rather than leaving the watch showing stale data.
        if (payload.size > negotiatedMtu - 3) {
            throw IllegalStateException(
                "payload ${payload.size} B exceeds MTU $negotiatedMtu (max ${negotiatedMtu - 3}); " +
                    "negotiation may not have completed"
            )
        }
        val c = when (uuid) {
            Protocol.CHAR_NOTIFICATION -> notifyChar
            Protocol.CHAR_CONTROL -> controlChar
            Protocol.CHAR_MEDIA -> mediaChar
            else -> g.getService(Protocol.SERVICE)?.getCharacteristic(uuid)
        } ?: throw IllegalStateException("Characteristic missing: $uuid")
        c.value = payload

        // Install the completion callback BEFORE asking the stack to send.
        // Registering it afterwards races: the ATT response can come back fast
        // enough that onCharacteristicWrite runs while pendingWrite is still
        // empty, so the callback is dropped and this coroutine waits forever.
        // That is what used to leave the watch stuck on a stale play/pause state.
        var lastError: String? = null
        for (attempt in 0 until WRITE_ATTEMPTS) {
            val outcome = withTimeoutOrNull(WRITE_CALLBACK_TIMEOUT_MS) {
                suspendCancellableCoroutine<Pair<Boolean, String?>> { cont ->
                    pendingWrite = { result ->
                        if (cont.isActive) {
                            result.fold(
                                onSuccess = { cont.resume(true to null) },
                                onFailure = { cont.resume(false to (it.message ?: "write failed")) },
                            )
                        }
                    }
                    cont.invokeOnCancellation { pendingWrite = null }
                    // Rejected means the TX queue is momentarily full; another
                    // write is completing or notifications are in flight.
                    val ok = try {
                        g.writeCharacteristic(c)
                    } catch (e: Exception) {
                        pendingWrite = null
                        false
                    }
                    if (!ok && cont.isActive) {
                        pendingWrite = null
                        cont.resume(false to "rejected")
                    }
                }
            }
            if (outcome != null) {
                val (ok, err) = outcome
                if (ok) {
                    appendLog("→ ${payload.toString(Charsets.UTF_8).ifEmpty { payload.joinToString() }}")
                    return
                }
                lastError = err
            } else {
                lastError = "callback timed out (link may have dropped)"
            }
            if (attempt < WRITE_ATTEMPTS - 1) delay(WRITE_RETRY_MS)
        }
        throw IllegalStateException("media/write failed: $lastError")
    }

    suspend fun sendNotification(title: String, body: String) {
        write(Protocol.CHAR_NOTIFICATION, Protocol.notificationPayload(title, body))
    }

    /** Push now-playing metadata so the watch's music face has something to show. */
    suspend fun sendMedia(title: String?, artist: String?, playing: Boolean, hasData: Boolean) {
        // The full payload can be 81 B, needing an MTU of 84+. If negotiation has
        // not completed yet the usable space is much smaller, so shrink the fields
        // to fit the current MTU instead of letting the write be dropped. The
        // header is 3 B plus 2 length bytes, leaving (mtu - 8) for text; the
        // title gets the larger share since it's the more useful on a 128x64 face.
        val usable = negotiatedMtu - 8
        if (usable <= 0) {
            throw IllegalStateException("MTU $negotiatedMtu too small for a media payload")
        }
        var t = title.orEmpty()
        var a = artist.orEmpty()
        if (t.toByteArray(Charsets.UTF_8).size + a.toByteArray(Charsets.UTF_8).size > usable) {
            val tRoom = maxOf(1, usable * 2 / 3)
            t = Protocol.truncateUtf8(t, tRoom)
            a = Protocol.truncateUtf8(a, usable - tRoom)
        }
        write(
            Protocol.CHAR_MEDIA,
            Protocol.mediaPayload(t, a, playing, hasData),
        )
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
        distanceChar?.let { g.readCharacteristic(it) }
        caloriesChar?.let { g.readCharacteristic(it) }
    }
}
