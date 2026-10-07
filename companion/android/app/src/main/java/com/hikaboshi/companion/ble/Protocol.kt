package com.hikaboshi.companion.ble

import java.text.Normalizer
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
    val CHAR_MEDIA: UUID = UUID.fromString("d7e8f9a0-1b2c-4d5e-8f90-a1b2c3d4e5f6")

    const val FLAG_HAS_DATA = 0x01
    const val FLAG_HAS_UNREAD = 0x02

    const val MEDIA_FLAG_PLAYING = 0x01
    const val MEDIA_FLAG_HAS_DATA = 0x02
    const val MEDIA_TITLE_MAX = 48
    const val MEDIA_ARTIST_MAX = 32

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

    /** Turkish letters with no NFKD decomposition to plain ASCII (dotless/dotted I). */
    private val NON_DECOMPOSING_MAP = mapOf(
        'ı' to 'i', 'İ' to 'I',
    )

    /**
     * Watch font is ASCII-only and renders anything else as "?". Map Turkish
     * (and other accented Latin) letters to their closest ASCII form instead;
     * anything that still isn't ASCII afterwards (emoji, CJK, ...) is left as
     * "?" on the watch, same as before.
     */
    private fun toAsciiFriendly(s: String): String {
        val premapped = s.map { NON_DECOMPOSING_MAP[it] ?: it }.joinToString("")
        val decomposed = Normalizer.normalize(premapped, Normalizer.Form.NFKD)
        return decomposed.replace(Regex("\\p{Mn}+"), "")
    }

    /**
     * Truncate to at most [maxBytes] UTF-8 bytes without splitting a code
     * point. Rewinds over trailing continuation bytes (10xxxxxx) so we never
     * cut a multi-byte character in half.
     */
    fun truncateUtf8(s: String, maxBytes: Int): String {
        val b = s.toByteArray(Charsets.UTF_8)
        if (b.size <= maxBytes) return s
        var end = maxBytes
        while (end > 0 && b[end - 1].toInt() and 0xC0 == 0x80) end--
        return String(b, 0, end, Charsets.UTF_8)
    }

    /** Title|Body payload, truncated to the firmware's field limits (UTF-8 safe). */
    fun notificationPayload(title: String, body: String): ByteArray {
        // Firmware caps: title 31 B, body 127 B.
        val asciiTitle = toAsciiFriendly(title)
        val asciiBody = toAsciiFriendly(body)
        return "${truncateUtf8(asciiTitle, 31)}|${truncateUtf8(asciiBody, 127)}"
            .toByteArray(Charsets.UTF_8)
    }

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

    /**
     * Build a Media characteristic payload (phone → watch).
     *
     * Wire format mirrors the watch's own encoder:
     *   byte 0: flags (bit0=playing, bit1=has_data)
     *   byte 1: title_len
     *   byte 2: artist_len
     *   bytes 3..: title bytes, then artist bytes
     *
     * Title/artist are truncated to the watch's buffer sizes. Non-ASCII is
     * kept (the watch renders UTF-8); lengths are counted in *bytes*, not
     * characters, so multi-byte text cannot desync the parser.
     */
    fun mediaPayload(
        title: String?,
        artist: String?,
        playing: Boolean,
        hasData: Boolean,
    ): ByteArray {
        val t = truncateUtf8(title.orEmpty(), MEDIA_TITLE_MAX - 1)
        val a = truncateUtf8(artist.orEmpty(), MEDIA_ARTIST_MAX - 1)
        val tb = t.toByteArray(Charsets.UTF_8)
        val ab = a.toByteArray(Charsets.UTF_8)
        var flags = 0
        if (playing) flags = flags or MEDIA_FLAG_PLAYING
        if (hasData) flags = flags or MEDIA_FLAG_HAS_DATA
        val out = ByteArray(3 + tb.size + ab.size)
        out[0] = flags.toByte()
        out[1] = tb.size.toByte()
        out[2] = ab.size.toByte()
        tb.copyInto(out, 3)
        ab.copyInto(out, 3 + tb.size)
        return out
    }

    data class WatchMedia(
        val hasData: Boolean,
        val playing: Boolean,
        val title: String,
        val artist: String,
    )

    /** Parse Media characteristic READ payload. */
    fun parseMedia(data: ByteArray): WatchMedia? {
        if (data.size < 3) return null
        val flags = data[0].toInt() and 0xFF
        val titleLen = data[1].toInt() and 0xFF
        val artistLen = data[2].toInt() and 0xFF
        if (data.size < 3 + titleLen + artistLen) return null
        return WatchMedia(
            hasData = flags and MEDIA_FLAG_HAS_DATA != 0,
            playing = flags and MEDIA_FLAG_PLAYING != 0,
            title = String(data, 3, titleLen, Charsets.UTF_8),
            artist = String(data, 3 + titleLen, artistLen, Charsets.UTF_8),
        )
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
    const val EVT_DISMISS_NOTIF = "dismiss_notif"
    const val EVT_DND_TOGGLE = "dnd_toggle"
}
