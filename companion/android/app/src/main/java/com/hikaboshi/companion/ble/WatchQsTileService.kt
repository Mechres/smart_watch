package com.hikaboshi.companion.ble

import android.app.PendingIntent
import android.content.Intent
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import com.hikaboshi.companion.MainActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/** Quick Settings shade tile: shows link status, tap to connect/disconnect. */
class WatchQsTileService : TileService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var job: Job? = null

    override fun onStartListening() {
        super.onStartListening()
        val ble = BleHolder.get(this)
        updateTile(ble.state.value)
        job = scope.launch {
            ble.state.collect { updateTile(it) }
        }
    }

    override fun onStopListening() {
        job?.cancel()
        job = null
        super.onStopListening()
    }

    override fun onClick() {
        super.onClick()
        val ble = BleHolder.get(this)
        val s = ble.state.value
        when {
            s.connected -> {
                ble.disconnect()
                WatchLinkService.stop(this)
            }
            s.lastAddress != null -> WatchLinkService.start(this, s.lastAddress)
            else -> {
                val intent = Intent(this, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                val pi = PendingIntent.getActivity(
                    this, 0, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
                )
                startActivityAndCollapse(pi)
            }
        }
    }

    private fun updateTile(state: WatchState) {
        val tile = qsTile ?: return
        tile.state = if (state.connected) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        tile.label = "Hikaboshi"
        tile.subtitle = when {
            state.connected -> state.battery?.let { "Battery $it%" } ?: "Connected"
            state.connecting -> "Connecting…"
            state.reconnecting -> "Reconnecting…"
            state.lastAddress == null -> "Not paired"
            else -> "Disconnected"
        }
        tile.updateTile()
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }
}
