package com.hikaboshi.companion.ble

import java.util.UUID

object Protocol {
    const val DEVICE_NAME = "Hikaboshi"

    val SERVICE: UUID = UUID.fromString("1d8a503d-e931-369f-164b-6f10596a178d")
    val CHAR_NOTIFICATION: UUID = UUID.fromString("2480757d-4f07-9fa5-0f48-e4125a9bdab8")
    val CHAR_CONTROL: UUID = UUID.fromString("b31cb75e-410c-29ba-0b45-9da7834df66e")
    val CHAR_BATTERY: UUID = UUID.fromString("12345678-90ab-cdef-1234-567890abcdef")
    val CHAR_STEPS: UUID = UUID.fromString("fedcba98-7654-3210-fedc-ba9876543210")
    val CHAR_DISTANCE: UUID = UUID.fromString("a1715cd1-0304-4b5c-b24a-111213141516")
    val CHAR_CALORIES: UUID = UUID.fromString("b2715cda-0506-4d5e-c35b-212223242526")

    const val FLAG_HAS_DATA = 0x01
    const val FLAG_HAS_UNREAD = 0x02

    data class WatchNotification(
        val hasData: Boolean,
        val hasUnread: Boolean,
        val title: String,
        val body: String,
    )

    /** Parse notification characteristic READ payload. */
    fun parseNotification(data: ByteArray): WatchNotification? {
        if (data.size < 3) return null
        val flags = data[0].toInt() and 0xFF
        val titleLen = data[1].toInt() and 0xFF
        val bodyLen = data[2].toInt() and 0xFF
        if (data.size < 3 + titleLen + bodyLen) return null
        val title = String(data, 3, titleLen, Charsets.UTF_8)
        val body = String(data, 3 + titleLen, bodyLen, Charsets.UTF_8)
        return WatchNotification(
            hasData = flags and FLAG_HAS_DATA != 0,
            hasUnread = flags and FLAG_HAS_UNREAD != 0,
            title = title,
            body = body,
        )
    }

    fun notificationPayload(title: String, body: String): ByteArray =
        "$title|$body".toByteArray(Charsets.UTF_8)

    fun parseBattery(data: ByteArray): Int? =
        data.firstOrNull()?.toInt()?.and(0xFF)

    fun parseSteps(data: ByteArray): Long? {
        if (data.size < 4) return null
        var v = 0L
        for (i in 0 until 4) {
            v = v or ((data[i].toLong() and 0xFF) shl (8 * i))
        }
        return v
    }

    /** Distance in whole meters (uint32 LE). */
    fun parseDistanceM(data: ByteArray): Long? {
        if (data.size < 4) return null
        var v = 0L
        for (i in 0 until 4) {
            v = v or ((data[i].toLong() and 0xFF) shl (8 * i))
        }
        return v
    }

    /** Calories in deci-kcal (uint32 LE, kcal × 10). Returns kcal as Double. */
    fun parseCaloriesKcal(data: ByteArray): Double? {
        if (data.size < 4) return null
        var v = 0L
        for (i in 0 until 4) {
            v = v or ((data[i].toLong() and 0xFF) shl (8 * i))
        }
        return v / 10.0
    }

    // Phone → watch
    fun cmdWifi(on: Boolean) = if (on) "wifi_on" else "wifi_off"
    fun cmdScreen(on: Boolean) = if (on) "screen_on" else "screen_off"
    fun cmdTime(unixSec: Long) = "time=$unixSec"
    fun cmdWeather(tempC: Double, code: Int) = "weather=$tempC,$code"
    fun cmdOta(url: String) = "ota=$url"

    // Watch → phone (control notifies)
    const val EVT_FIND_PHONE = "find_phone"
    const val EVT_FIND_PHONE_STOP = "find_phone_stop"
    const val EVT_MUSIC_TOGGLE = "music_toggle"
    const val EVT_MUSIC_NEXT = "music_next"
    const val EVT_MUSIC_PREV = "music_prev"
    const val EVT_ALARM = "alarm"
}
