package com.esp32rc.network

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import org.json.JSONObject
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

class WsClient(
    private val wsUrl: String,
    private val onFrame: (Bitmap) -> Unit,
    private val onError: (Exception) -> Unit = {},
    private val onStatus: (left: Int, right: Int) -> Unit = { _, _ -> }
) {

    private val client = OkHttpClient.Builder()
        .connectTimeout(3, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .pingInterval(5, TimeUnit.SECONDS)
        .build()

    private val sequence = AtomicLong(System.currentTimeMillis())
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    private var running = false

    @Volatile
    private var ws: WebSocket? = null

    private var reconnectAttempt = 0

    fun connect() {
        running = true
        reconnectAttempt = 0
        doConnect()
    }

    fun sendCommand(left: Int, right: Int) {
        val seq = sequence.incrementAndGet()
        ws?.send("$left,$right,$seq")
    }

    fun disconnect() {
        running = false
        ws?.close(1000, "bye")
        ws = null
    }

    fun shutdown() {
        disconnect()
        client.dispatcher.executorService.shutdown()
        client.connectionPool.evictAll()
    }

    private fun doConnect() {
        if (!running) return
        val request = Request.Builder().url(wsUrl).build()
        ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                Log.i("RC-CAR", "WS connected")
                reconnectAttempt = 0
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                val data = bytes.toByteArray()
                val bitmap = BitmapFactory.decodeByteArray(data, 0, data.size)
                if (bitmap != null) {
                    mainHandler.post { onFrame(bitmap) }
                }
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                try {
                    val json = JSONObject(text)
                    val left = json.optInt("l", 0)
                    val right = json.optInt("r", 0)
                    mainHandler.post { onStatus(left, right) }
                } catch (_: Exception) {
                }
            }

            override fun onFailure(
                webSocket: WebSocket,
                t: Throwable,
                response: Response?
            ) {
                Log.w("RC-CAR", "WS failure: $t")
                ws = null
                if (running) {
                    mainHandler.post { onError(t as? Exception ?: Exception(t)) }
                    scheduleReconnect()
                }
            }

            override fun onClosed(
                webSocket: WebSocket,
                code: Int,
                reason: String
            ) {
                Log.i("RC-CAR", "WS closed: $code $reason")
                ws = null
                if (running) scheduleReconnect()
            }
        })
    }

    private fun scheduleReconnect() {
        reconnectAttempt++
        val delay = minOf(500L * reconnectAttempt, 5000L)
        Thread {
            Thread.sleep(delay)
            if (running) doConnect()
        }.start()
    }
}
