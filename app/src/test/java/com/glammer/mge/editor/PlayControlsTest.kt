package com.glammer.mge.editor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PlayControlsTest {
    // Captured from EditorSession::stateJson: starter scene, Player selected, play, 30 frames
    // at 60 Hz, pause, one step.
    private val playing = javaClass.classLoader!!.getResource("editor_state_playing.json")!!.readText()

    @Test
    fun parsesPlayStateAndEnumFields() {
        val s = EditorState.parse(playing)
        assertEquals(PlayState.Paused, s.play.state)
        assertTrue(s.play.active)
        assertTrue(s.locked)
        assertEquals(0.52f, s.play.time, 1e-4f)
        assertEquals(31L, s.play.steps)
        val body = s.inspector!!.components.first { it.type == "Collider2D" }.fields.first { it.name == "bodyType" }
        assertEquals(FieldHint.Enum, body.hint)
        assertEquals(listOf("Static", "Kinematic", "Dynamic"), body.options)
        assertEquals("Dynamic", body.value)
        assertEquals(0, body.parts)
        // Older engines send no play object: editing.
        assertEquals(PlayInfo(), EditorState.parse("{}").play)
    }

    @Test
    fun controlsFollowPlayState() {
        val editing = PlayControls(PlayInfo())
        assertEquals("play", editing.primary)
        assertEquals("Play", editing.primaryLabel)
        assertFalse(editing.canStep)
        assertFalse(editing.canStop)
        assertEquals("", editing.clock)

        val running = PlayControls(PlayInfo(PlayState.Playing, 1.5f, 90))
        assertEquals("pause", running.primary)
        assertEquals("⏸", running.primaryGlyph)
        assertFalse(running.canStep)
        assertTrue(running.canStop)
        assertEquals("1.50 s · 90 steps", running.clock)

        val paused = PlayControls(PlayInfo(PlayState.Paused, 0.52f, 31))
        assertEquals("play", paused.primary)
        assertEquals("Resume", paused.primaryLabel)
        assertTrue(paused.canStep)
        assertTrue(paused.canStop)
    }

    @Test
    fun buildsPlayCommands() {
        assertEquals(listOf("play", "pause", "step", "stop"),
            listOf(EditorCommands.play(), EditorCommands.pause(), EditorCommands.step(), EditorCommands.stop()))
    }
}
