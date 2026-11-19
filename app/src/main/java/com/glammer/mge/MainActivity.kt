package com.glammer.mge

import android.annotation.SuppressLint
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.glammer.mge.editor.EditorOverlay
import com.glammer.mge.editor.EditorState
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {

    private val surfaceCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) = NativeEngine.surfaceCreated(holder.surface)
        override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) =
            NativeEngine.surfaceChanged(w, h)
        override fun surfaceDestroyed(holder: SurfaceHolder) = NativeEngine.surfaceDestroyed()
    }

    @SuppressLint("ClickableViewAccessibility")   // the viewport is a game surface, not a button
    private val engineTouchListener = android.view.View.OnTouchListener { _, event ->
        NativeEngine.sendTouch(event)
        true
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Draw under the system bars and cutout; the engine keeps gameplay inside the safe area.
        enableEdgeToEdge()
        NativeEngine.create(assets, filesDir.absolutePath)
        NativeEngine.setDensity(resources.displayMetrics.density)
        setContent {
            Box(Modifier.fillMaxSize()) {
                // Engine viewport
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { ctx ->
                        SurfaceView(ctx).apply {
                            holder.addCallback(surfaceCallback)
                            // Touches go straight to the engine thread; gestures are recognised natively.
                            setOnTouchListener(engineTouchListener)
                            ViewCompat.setOnApplyWindowInsetsListener(this) { _, insets ->
                                val safe = insets.getInsets(
                                    WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
                                )
                                NativeEngine.setInsets(safe.left, safe.top, safe.right, safe.bottom)
                                insets
                            }
                        }
                    }
                )
                // Editor chrome (M2): panels over the viewport. The engine publishes its state;
                // the UI re-reads it only when the revision moves (cheap to poll at 30 Hz).
                var editor by remember { mutableStateOf(EditorState()) }
                LaunchedEffect(Unit) {
                    while (true) {
                        NativeEngine.editorState(editor.revision)?.let { json ->
                            runCatching { EditorState.parse(json) }.onSuccess { editor = it }
                        }
                        delay(33)
                    }
                }
                if (editor.revision >= 0) {
                    EditorOverlay(
                        state = editor,
                        send = NativeEngine::editorCommand,
                        modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(bottom = 48.dp)
                    )
                } else {
                    Text(
                        "MobileGlammerEngine - tap: drop, hold: blast, drag/pinch: camera",
                        color = Color.White,
                        fontSize = 12.sp,
                        modifier = Modifier.align(Alignment.TopStart).safeDrawingPadding().padding(12.dp)
                    )
                }
                // Live engine stats, in their own composable so the half-second refresh
                // recomposes only this text, never the editor panels (that hitched scrolling).
                EngineStats(Modifier.align(Alignment.BottomStart).safeDrawingPadding().padding(12.dp))
            }
        }
    }

    /** Frame time, draw calls, bodies and memory, polled twice a second. */
    @Composable
    private fun EngineStats(modifier: Modifier) {
        var stats by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            while (true) {
                stats = NativeEngine.stats()
                delay(500)
            }
        }
        Text(
            stats,
            color = Color(0xFFB8F5C0),
            fontSize = 11.sp,
            lineHeight = 14.sp,
            fontFamily = FontFamily.Monospace,
            modifier = modifier,
        )
    }

    override fun onResume() { super.onResume(); NativeEngine.resume() }
    override fun onPause() { NativeEngine.pause(); super.onPause() }
    override fun onDestroy() { if (isFinishing) NativeEngine.destroy(); super.onDestroy() }
}
