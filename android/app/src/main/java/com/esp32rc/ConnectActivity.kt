package com.esp32rc

import android.content.Intent
import android.os.Bundle
import android.util.Patterns
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.snackbar.Snackbar
import com.google.android.material.textfield.TextInputEditText
import okhttp3.Call
import okhttp3.Callback
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import java.io.IOException
import java.util.concurrent.TimeUnit

class ConnectActivity : AppCompatActivity() {

    companion object {
        private const val PREFS = "connect"
        private const val KEY_IP = "ip"
        private const val KEY_PORT = "port"
    }

    private lateinit var ipInput: TextInputEditText
    private lateinit var portInput: TextInputEditText
    private lateinit var connectButton: MaterialButton
    private lateinit var statusText: TextView

    private val client = OkHttpClient.Builder()
        .connectTimeout(3, TimeUnit.SECONDS)
        .readTimeout(3, TimeUnit.SECONDS)
        .build()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_connect)

        ipInput = findViewById(R.id.ipInput)
        portInput = findViewById(R.id.portInput)
        connectButton = findViewById(R.id.connectButton)
        statusText = findViewById(R.id.statusText)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        ipInput.setText(prefs.getString(KEY_IP, "192.168.4.1"))
        portInput.setText(prefs.getInt(KEY_PORT, 80).toString())

        connectButton.setOnClickListener { attemptConnect() }
    }

    private fun attemptConnect() {
        val ip = ipInput.text?.toString()?.trim().orEmpty()
        val port = portInput.text?.toString()?.trim()?.toIntOrNull()

        if (ip.isEmpty() || !Patterns.IP_ADDRESS.matcher(ip).matches()) {
            showError(getString(R.string.error_invalid_ip))
            return
        }
        if (port == null || port !in 1..65535) {
            showError(getString(R.string.error_invalid_port))
            return
        }

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_IP, ip)
            .putInt(KEY_PORT, port)
            .apply()

        startControl(ip, port)
    }

    private fun startControl(ip: String, port: Int) {
        startActivity(Intent(this, ControlActivity::class.java).apply {
            putExtra(ControlActivity.EXTRA_IP, ip)
            putExtra(ControlActivity.EXTRA_PORT, port)
        })
    }

    private fun showError(message: String) {
        Snackbar.make(connectButton, message, Snackbar.LENGTH_LONG).show()
    }
}
