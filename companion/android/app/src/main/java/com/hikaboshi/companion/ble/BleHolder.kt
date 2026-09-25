package com.hikaboshi.companion.ble

import android.content.Context

/**
 * App-scoped singleton owner of the BLE manager.
 * The manager must outlive any single Activity (boot connect, background
 * reconnect, notification forwarding), so it lives here on the
 * application context — never on an Activity context.
 */
object BleHolder {
    const val PREFS = "hikaboshi"
    const val KEY_ADDRESS = "last_address"
    const val KEY_AUTO_CONNECT = "auto_connect"
    const val KEY_FORWARD = "forward_notifications"
    const val KEY_AUTO_WEATHER = "auto_weather"
    const val KEY_NOTIF_DISABLED_APPS = "notif_disabled_apps"

    @Volatile
    private var manager: WatchBleManager? = null

    fun get(context: Context): WatchBleManager {
        return manager ?: synchronized(this) {
            manager ?: WatchBleManager(context.applicationContext).also { manager = it }
        }
    }

    /** Null when nothing ever created the manager (e.g. listener before first launch). */
    fun peek(): WatchBleManager? = manager

    /** Packages excluded from notification forwarding (in addition to the fixed denylist). */
    fun disabledNotifApps(context: Context): Set<String> =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getStringSet(KEY_NOTIF_DISABLED_APPS, emptySet()) ?: emptySet()

    fun setNotifAppEnabled(context: Context, packageName: String, enabled: Boolean) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val current = (prefs.getStringSet(KEY_NOTIF_DISABLED_APPS, emptySet()) ?: emptySet())
            .toMutableSet()
        if (enabled) current.remove(packageName) else current.add(packageName)
        prefs.edit().putStringSet(KEY_NOTIF_DISABLED_APPS, current).apply()
    }
}
