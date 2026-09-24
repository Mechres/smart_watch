package com.hikaboshi.companion.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.BrightnessHigh
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Send
import androidx.compose.material.icons.filled.Smartphone
import androidx.compose.material.icons.filled.SystemUpdate
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WatchApp(vm: WatchViewModel, state: UiState) {
    val snackbar = remember { SnackbarHostState() }

    LaunchedEffect(state.toast) {
        state.toast?.let {
            snackbar.showSnackbar(it)
            vm.clearToast()
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("Hikaboshi")
                        Text(
                            if (state.ble.connected) {
                                "Connected ${state.ble.address ?: ""}"
                            } else if (state.ble.connecting) {
                                "Connecting…"
                            } else {
                                "Disconnected"
                            },
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                },
                actions = {
                    if (state.ble.connected) {
                        IconButton(onClick = { vm.refresh() }) {
                            Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                        }
                        IconButton(onClick = { vm.disconnect() }) {
                            Icon(Icons.Default.Close, contentDescription = "Disconnect")
                        }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 12.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (!state.ble.connected) {
                ScanSection(state, vm)
            } else {
                DashboardSection(state, vm)
            }
        }
    }
}

@Composable
private fun ScanSection(state: UiState, vm: WatchViewModel) {
    Column(Modifier.fillMaxWidth()) {
        Row(
            Modifier.fillMaxWidth().padding(top = 8.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Devices", style = MaterialTheme.typography.titleMedium)
            Row {
                OutlinedButton(onClick = { vm.scan() }, enabled = !state.ble.scanning) {
                    Icon(Icons.Default.Bluetooth, null)
                    Spacer(Modifier.height(4.dp))
                    Text(if (state.ble.scanning) "Scanning…" else "Scan")
                }
                if (state.ble.scanning) {
                    Spacer(Modifier.padding(4.dp))
                    OutlinedButton(onClick = { vm.stopScan() }) { Text("Stop") }
                }
            }
        }

        state.ble.error?.let { err ->
            Text(
                err,
                modifier = Modifier.padding(top = 8.dp),
                color = MaterialTheme.colorScheme.error,
            )
        }

        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            state.ble.scanResults.forEach { dev ->
                Card(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.fillMaxWidth().padding(12.dp),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column {
                            Text(dev.name ?: "(no name)", style = MaterialTheme.typography.titleSmall)
                            Text(dev.address, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                            Text("RSSI ${dev.rssi}", style = MaterialTheme.typography.bodySmall)
                        }
                        Button(
                            onClick = { vm.connect(dev.address) },
                            enabled = !state.busy,
                        ) { Text("Connect") }
                    }
                }
            }
        }

        if (state.ble.scanResults.isEmpty() && !state.ble.scanning) {
            Text(
                "Wake the watch (raise wrist) then tap Scan.\nAdvertising stops after ~5 min idle.",
                Modifier.padding(top = 24.dp),
                style = MaterialTheme.typography.bodyMedium,
            )
        }

        EventLogCard(state, Modifier.padding(top = 12.dp))
    }
}

@Composable
private fun EventLogCard(state: UiState, modifier: Modifier = Modifier) {
    Card(modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp)) {
            Text("Event log", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(4.dp))
            if (state.history.isEmpty() && state.ble.log.isEmpty()) {
                Text("—", style = MaterialTheme.typography.bodySmall)
            } else {
                val lines = (state.ble.log + state.history).takeLast(40).reversed()
                lines.forEach { line ->
                    Text(
                        line,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        }
    }
}

@Composable
private fun DashboardSection(state: UiState, vm: WatchViewModel) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // Status
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Status", style = MaterialTheme.typography.titleMedium)
                Text("Battery: ${state.ble.battery?.let { "$it%" } ?: "—"}")
                Text("Steps: ${state.ble.steps?.toString() ?: "—"}")
                state.ble.lastNotification?.let { n ->
                    Text("Last notif: ${n.title} — ${n.body}")
                }
                state.ble.lastOtaStatus?.let {
                    Text("OTA: $it ${state.ble.lastOtaProgress?.let { p -> "($p%)" } ?: ""}")
                }
            }
        }

        // Send notification
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Send notification", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = state.notifTitle,
                    onValueChange = vm::setNotifTitle,
                    label = { Text("Title") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = state.notifBody,
                    onValueChange = vm::setNotifBody,
                    label = { Text("Body") },
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(onClick = vm::sendNotification, enabled = !state.busy) {
                    Icon(Icons.Default.Send, null)
                    Text("  Send")
                }
            }
        }

        // Controls
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Controls", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { vm.screen(true) }, enabled = !state.busy) {
                        Icon(Icons.Default.BrightnessHigh, null); Text(" Screen on")
                    }
                    OutlinedButton(onClick = { vm.screen(false) }, enabled = !state.busy) {
                        Icon(Icons.Default.Close, null); Text(" Screen off")
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { vm.wifi(true) }, enabled = !state.busy) {
                        Icon(Icons.Default.Wifi, null); Text(" WiFi on")
                    }
                    OutlinedButton(onClick = { vm.wifi(false) }, enabled = !state.busy) {
                        Icon(Icons.Default.Wifi, null); Text(" WiFi off")
                    }
                }
                OutlinedButton(onClick = vm::setTimeSync, enabled = !state.busy) {
                    Icon(Icons.Default.Check, null); Text(" Sync time")
                }
            }
        }

        // Weather
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Weather", style = MaterialTheme.typography.titleMedium)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = state.weatherTemp,
                        onValueChange = vm::setWeatherTemp,
                        label = { Text("°C") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedTextField(
                        value = state.weatherCode,
                        onValueChange = vm::setWeatherCode,
                        label = { Text("Code") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                }
                Button(onClick = vm::sendWeather, enabled = !state.busy) {
                    Icon(Icons.Default.Cloud, null); Text("  Push weather")
                }
            }
        }

        // OTA
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("OTA firmware", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = state.otaUrl,
                    onValueChange = vm::setOtaUrl,
                    label = { Text("https://…/smart_watch.bin") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(onClick = vm::sendOta, enabled = !state.busy) {
                    Icon(Icons.Default.SystemUpdate, null); Text("  Start OTA")
                }
                state.ble.lastOtaProgress?.let { p ->
                    Text("Progress: $p%")
                }
            }
        }

        EventLogCard(state)

        // Find phone / music hint
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp)) {
                Text("From the watch", style = MaterialTheme.typography.titleMedium)
                Text(
                    "Find Phone → rings this phone.\n" +
                        "Music Control → play/next/prev media keys.\n" +
                    "Keep this app open (or the link notification) for events.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}
