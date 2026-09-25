package com.hikaboshi.companion.ble

import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationManager
import android.os.CancellationSignal
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.Dispatchers
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import kotlin.coroutines.resume

/** Device-location current weather via Open-Meteo (free, no API key, WMO codes). */
object WeatherFetcher {
    private const val TAG = "WatchBle"

    data class Result(val tempC: Double, val wmoCode: Int)

    private fun hasLocationPermission(context: Context): Boolean =
        ContextCompat.checkSelfPermission(
            context, android.Manifest.permission.ACCESS_COARSE_LOCATION,
        ) == PackageManager.PERMISSION_GRANTED

    private suspend fun currentLocation(context: Context): Location? {
        if (!hasLocationPermission(context)) return null
        val lm = context.getSystemService(LocationManager::class.java) ?: return null
        val provider = when {
            lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER) -> LocationManager.NETWORK_PROVIDER
            lm.isProviderEnabled(LocationManager.GPS_PROVIDER) -> LocationManager.GPS_PROVIDER
            else -> return null
        }
        val fresh = suspendCancellableCoroutine<Location?> { cont ->
            val cancel = CancellationSignal()
            cont.invokeOnCancellation { cancel.cancel() }
            try {
                lm.getCurrentLocation(
                    provider,
                    cancel,
                    ContextCompat.getMainExecutor(context),
                ) { loc -> if (cont.isActive) cont.resume(loc) }
            } catch (e: SecurityException) {
                if (cont.isActive) cont.resume(null)
            }
        }
        return fresh ?: try {
            lm.getLastKnownLocation(provider)
        } catch (e: SecurityException) {
            null
        }
    }

    /** Null on any failure (no permission, no location fix, no network). */
    suspend fun fetchForDeviceLocation(context: Context): Result? = withContext(Dispatchers.IO) {
        val loc = currentLocation(context) ?: run {
            Log.w(TAG, "Auto-weather: no location fix")
            return@withContext null
        }
        try {
            val url = URL(
                "https://api.open-meteo.com/v1/forecast" +
                    "?latitude=${loc.latitude}&longitude=${loc.longitude}&current_weather=true",
            )
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 10_000
            conn.readTimeout = 10_000
            conn.requestMethod = "GET"
            val body = conn.inputStream.bufferedReader().use { it.readText() }
            conn.disconnect()
            val current = JSONObject(body).getJSONObject("current_weather")
            Result(
                tempC = current.getDouble("temperature"),
                wmoCode = current.getInt("weathercode"),
            )
        } catch (e: Exception) {
            Log.w(TAG, "Auto-weather fetch failed: ${e.message}")
            null
        }
    }
}
