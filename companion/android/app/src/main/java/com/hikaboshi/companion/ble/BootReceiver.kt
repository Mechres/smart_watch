package com.hikaboshi.companion.ble

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/** Re-establishes the watch link after a reboot when auto-connect is on. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        val prefs = context.getSharedPreferences(BleHolder.PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(BleHolder.KEY_AUTO_CONNECT, true)) return
        val address = prefs.getString(BleHolder.KEY_ADDRESS, null) ?: return
        Log.d("WatchBle", "Boot: starting link service for $address")
        try {
            WatchLinkService.start(context.applicationContext, address)
        } catch (e: Exception) {
            Log.w("WatchBle", "Boot start failed: ${e.message}")
        }
    }
}
