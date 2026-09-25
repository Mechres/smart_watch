package com.hikaboshi.companion.ui

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.hikaboshi.companion.ble.Protocol
import com.hikaboshi.companion.ble.WatchBleManager
import com.hikaboshi.companion.ble.WatchLinkService
import com.hikaboshi.companion.ble.WatchState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

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
        ble.onControlEvent = { cmd -> handleWatchEvent(cmd) }
    }

    private fun handleWatchEvent(cmd: String) {
        when (cmd) {
            Protocol.EVT_FIND_PHONE,
            Protocol.EVT_FIND_PHONE_STOP,
            Protocol.EVT_MUSIC_TOGGLE,
            Protocol.EVT_MUSIC_NEXT,
            Protocol.EVT_MUSIC_PREV,
            -> {
                _ui.value = _ui.value.copy(
                    history = (_ui.value.history + "watch: $cmd").takeLast(100),
                )
                // Service may also be listening; dual-dispatch is fine.
                WatchLinkService.instance?.let { /* already hooked via attach */ }
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
