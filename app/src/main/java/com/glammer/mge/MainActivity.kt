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
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

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
                // Editor chrome will live in Compose on top of the viewport
                Text(
                    "MobileGlammerEngine M1",
                    color = Color.White,
                    modifier = Modifier.align(Alignment.TopStart).padding(16.dp)
                )
            }
        }
    }

    override fun onResume() { super.onResume(); NativeEngine.resume() }
    override fun onPause() { NativeEngine.pause(); super.onPause() }
    override fun onDestroy() { if (isFinishing) NativeEngine.destroy(); super.onDestroy() }
}
