package com.hikaboshi.companion.ui

import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.provider.Settings
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.hikaboshi.companion.ble.BleHolder
import com.hikaboshi.companion.ble.Protocol
import com.hikaboshi.companion.ble.WatchBleManager
import com.hikaboshi.companion.ble.WatchLinkService
import com.hikaboshi.companion.ble.WatchState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** One row in the per-app notification-forwarding picker. */
data class NotifAppRow(
    val packageName: String,
    val label: String,
    val enabled: Boolean,
)

/** UI-only fields layered on top of WatchState. */
data class UiState(
    val ble: WatchState = WatchState(),
    val notifTitle: String = "",
    val notifBody: String = "",
    val weatherTemp: String = "20",
    val weatherCode: String = "1",
    val otaUrl: String = "",
    val history: List<String> = emptyList(),
    val busy: Boolean = false,
    val toast: String? = null,
    val forwardEnabled: Boolean = false,
    val listenerGranted: Boolean = false,
    val dndAccessGranted: Boolean = false,
    val notifApps: List<NotifAppRow> = emptyList(),
    val showAppPicker: Boolean = false,
)

class WatchViewModelFactory(
    private val ble: WatchBleManager,
    private val appContext: Context,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T {
        return WatchViewModel(ble, appContext) as T
    }
}

class WatchViewModel(
    private val ble: WatchBleManager,
    private val appContext: Context,
) : ViewModel() {

    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    init {
        _ui.value = _ui.value.copy(
            forwardEnabled = ble.forwardingEnabled(),
            listenerGranted = isListenerGranted(),
            dndAccessGranted = isDndAccessGranted(),
        )
        viewModelScope.launch {
            ble.state.collect { s ->
                _ui.value = _ui.value.copy(ble = s)
                // Record OTA / control lines into history
                s.lastControlMessage?.let { msg ->
                    if (_ui.value.history.lastOrNull() != msg) {
                        _ui.value = _ui.value.copy(
                            history = (_ui.value.history + msg).takeLast(100),
                        )
                    }
                }
            }
        }
        viewModelScope.launch {
            ble.controlEvents.collect { cmd -> handleWatchEvent(cmd) }
        }
    }

    private fun handleWatchEvent(cmd: String) {
        when (cmd) {
            Protocol.EVT_FIND_PHONE,
            Protocol.EVT_FIND_PHONE_STOP,
            Protocol.EVT_MUSIC_TOGGLE,
            Protocol.EVT_MUSIC_NEXT,
            Protocol.EVT_MUSIC_PREV,
            Protocol.EVT_DISMISS_NOTIF,
            Protocol.EVT_DND_TOGGLE,
            -> {
                _ui.value = _ui.value.copy(
                    history = (_ui.value.history + "watch: $cmd").takeLast(100),
                )
            }
            Protocol.EVT_ALARM -> {
                _ui.value = _ui.value.copy(
                    history = (_ui.value.history + "watch: $cmd").takeLast(100),
                    toast = "Alarm from watch",
                )
            }
        }
    }

    fun scan() = ble.startScan()
    fun stopScan() = ble.stopScan()

    fun connect(address: String) {
        viewModelScope.launch {
            _ui.value = _ui.value.copy(busy = true, toast = null)
            try {
                ble.connect(address)
                WatchLinkService.start(appContext, address, ble)
                _ui.value = _ui.value.copy(toast = "Connected")
            } catch (e: Exception) {
                _ui.value = _ui.value.copy(toast = "Connect failed: ${e.message}")
            } finally {
                _ui.value = _ui.value.copy(busy = false)
            }
        }
    }

    fun disconnect() {
        ble.disconnect()
        WatchLinkService.sharedManager = null
        WatchLinkService.stop(appContext)
        _ui.value = _ui.value.copy(toast = "Disconnected")
    }

    /** Auto-connect on app start when a device is remembered. */
    fun autoStart() {
        val s = ble.state.value
        val addr = s.lastAddress
        if (s.autoConnect && addr != null && !s.connected && !s.connecting) {
            connect(addr)
        }
    }

    fun setAutoConnect(enabled: Boolean) {
        ble.setAutoConnect(enabled)
    }

    fun forgetDevice() {
        ble.forgetDevice()
        WatchLinkService.stop(appContext)
        _ui.value = _ui.value.copy(toast = "Device forgotten")
    }

    fun setForwarding(enabled: Boolean) {
        ble.setForwarding(enabled)
        _ui.value = _ui.value.copy(forwardEnabled = enabled)
    }

    fun openListenerSettings() {
        val intent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            appContext.startActivity(intent)
        } catch (e: Exception) {
            _ui.value = _ui.value.copy(toast = "Cannot open settings: ${e.message}")
        }
    }

    /** Re-check system listener/DND access (call from onResume). */
    fun refreshListenerState() {
        _ui.value = _ui.value.copy(
            listenerGranted = isListenerGranted(),
            forwardEnabled = ble.forwardingEnabled(),
            dndAccessGranted = isDndAccessGranted(),
        )
    }

    private fun isListenerGranted(): Boolean {
        val pkgs = NotificationManagerCompat.getEnabledListenerPackages(appContext)
        return pkgs.contains(appContext.packageName)
    }

    fun openDndSettings() {
        val intent = Intent(Settings.ACTION_NOTIFICATION_POLICY_ACCESS_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        try {
            appContext.startActivity(intent)
        } catch (e: Exception) {
            _ui.value = _ui.value.copy(toast = "Cannot open settings: ${e.message}")
        }
    }

    private fun isDndAccessGranted(): Boolean {
        val nm = appContext.getSystemService(NotificationManager::class.java) ?: return false
        return nm.isNotificationPolicyAccessGranted
    }

    fun hasLocationPermission(): Boolean =
        ContextCompat.checkSelfPermission(
            appContext, android.Manifest.permission.ACCESS_COARSE_LOCATION,
        ) == PackageManager.PERMISSION_GRANTED

    fun setAutoWeather(enabled: Boolean) {
        ble.setAutoWeather(enabled)
    }

    fun refreshWeatherNow() {
        safeLaunch("Weather refreshed") {
            if (!ble.refreshWeatherNow()) throw IllegalStateException("no location/network")
        }
    }

    fun openAppPicker() {
        _ui.value = _ui.value.copy(showAppPicker = true)
        if (_ui.value.notifApps.isEmpty()) loadNotifApps()
    }

    fun closeAppPicker() {
        _ui.value = _ui.value.copy(showAppPicker = false)
    }

    fun setNotifAppEnabled(packageName: String, enabled: Boolean) {
        BleHolder.setNotifAppEnabled(appContext, packageName, enabled)
        _ui.value = _ui.value.copy(
            notifApps = _ui.value.notifApps.map {
                if (it.packageName == packageName) it.copy(enabled = enabled) else it
            },
        )
    }

    private fun loadNotifApps() {
        viewModelScope.launch {
            val rows = withContext(Dispatchers.Default) {
                val pm = appContext.packageManager
                val disabled = BleHolder.disabledNotifApps(appContext)
                pm.getInstalledApplications(PackageManager.GET_META_DATA)
                    .filter {
                        it.packageName != appContext.packageName &&
                            (pm.getLaunchIntentForPackage(it.packageName) != null ||
                                it.flags and ApplicationInfo.FLAG_SYSTEM == 0)
                    }
                    .map { app ->
                        NotifAppRow(
                            packageName = app.packageName,
                            label = pm.getApplicationLabel(app).toString(),
                            enabled = app.packageName !in disabled,
                        )
                    }
                    .sortedBy { it.label.lowercase() }
            }
            _ui.value = _ui.value.copy(notifApps = rows)
        }
    }

    fun refresh() = ble.refreshReads()

    fun setNotifTitle(v: String) {
        _ui.value = _ui.value.copy(notifTitle = v)
    }

    fun setNotifBody(v: String) {
        _ui.value = _ui.value.copy(notifBody = v)
    }

    fun sendNotification() {
        val t = _ui.value.notifTitle.ifBlank { "Message" }
        val b = _ui.value.notifBody
        safeLaunch("Notification sent") { ble.sendNotification(t, b) }
    }

    fun sendPreset(body: String) {
        safeLaunch("Sent") { ble.sendNotification("Hikaboshi", body) }
    }

    fun setTimeSync() = safeLaunch("Time synced") { ble.sendTimeSync() }

    fun setWeatherTemp(v: String) {
        _ui.value = _ui.value.copy(weatherTemp = v)
    }

    fun setWeatherCode(v: String) {
        _ui.value = _ui.value.copy(weatherCode = v)
    }

    fun sendWeather() {
        val temp = _ui.value.weatherTemp.toDoubleOrNull() ?: return
        val code = _ui.value.weatherCode.toIntOrNull() ?: 1
        safeLaunch("Weather sent") { ble.sendWeather(temp, code) }
    }

    fun setOtaUrl(v: String) {
        _ui.value = _ui.value.copy(otaUrl = v)
    }

    fun sendOta() {
        val url = _ui.value.otaUrl.trim()
        if (!url.startsWith("https://")) {
            _ui.value = _ui.value.copy(toast = "OTA URL must be https://")
            return
        }
        safeLaunch("OTA started") { ble.sendOta(url) }
    }

    fun screen(on: Boolean) = safeLaunch(if (on) "Screen on" else "Screen off") {
        ble.sendScreen(on)
    }

    fun wifi(on: Boolean) = safeLaunch(if (on) "WiFi on" else "WiFi off") {
        ble.sendWifi(on)
    }

    fun clearToast() {
        _ui.value = _ui.value.copy(toast = null)
    }

    private fun safeLaunch(okMsg: String, block: suspend () -> Unit) {
        viewModelScope.launch {
            _ui.value = _ui.value.copy(busy = true, toast = null)
            try {
                block()
                _ui.value = _ui.value.copy(toast = okMsg)
            } catch (e: Exception) {
                _ui.value = _ui.value.copy(toast = "Error: ${e.message}")
            } finally {
                _ui.value = _ui.value.copy(busy = false)
            }
        }
    }
}
