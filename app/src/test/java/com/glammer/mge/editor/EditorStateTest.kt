package com.glammer.mge.editor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EditorStateTest {
    // Captured from EditorSession::stateJson (starter scene, Hat selected and renamed, rotate gizmo, snap on).
    private val fixture = javaClass.classLoader!!.getResource("editor_state.json")!!.readText()

    @Test
    fun parsesEngineState() {
        val s = EditorState.parse(fixture)
        assertTrue(s.revision > 0)
        assertEquals("Rename", s.undoLabel)
        assertTrue(s.canUndo)
        assertFalse(s.canRedo)
        assertTrue(s.dirty)
        assertEquals(listOf("Ground", "Player", "Hat \"red\" é", "Crate", "Crate (2)"), s.rows.map { it.name })
        val hat = s.rows[2]
        assertEquals(1, hat.depth)
        assertEquals(s.rows[1].id, hat.parent)
        assertTrue(hat.selected)
        assertTrue(s.rows[1].expanded)
        assertEquals(1, s.rows[1].childCount)
        assertEquals(listOf(hat.id), s.selection)
        assertEquals(GizmoSettings(GizmoMode.Rotate, local = false, snap = true, grid = 0.25f, angle = 15f, scaleStep = 0.1f), s.gizmo)
    }

    @Test
    fun parsesInspector() {
        val insp = EditorState.parse(fixture).inspector!!
        assertEquals("Hat \"red\" é", insp.name)
        assertEquals(listOf("Transform2D", "SpriteRenderer"), insp.components.map { it.type })
        assertFalse(insp.components[0].removable)
        val rotation = insp.components[0].fields.first { it.name == "rotation" }
        assertEquals(FieldType.F32, rotation.type)
        assertEquals(FieldHint.Angle, rotation.hint)
        assertEquals(1, rotation.parts)
        val position = insp.components[0].fields.first { it.name == "position" }
        assertEquals("0 0.7", position.value)
        assertEquals(2, position.parts)
        val color = insp.components[1].fields.first { it.name == "color" }
        assertEquals(FieldHint.Color, color.hint)
        assertEquals(0, color.parts)
        assertEquals("#FFB74DFF", color.value)
        assertEquals(4, insp.components[1].fields.first { it.name == "uvRect" }.parts)
        assertEquals(FieldHint.Bool, insp.components[1].fields.first { it.name == "visible" }.hint)
        assertTrue(insp.addable.isEmpty())
    }

    @Test
    fun emptySelectionAndUnknownTypes() {
        val s = EditorState.parse("""{"rev":3,"undo":"","redo":"Move","dirty":false,"locked":true,"rows":[],"sel":[],"insp":null}""")
        assertNull(s.inspector)
        assertTrue(s.canRedo)
        assertTrue(s.locked)
        assertEquals(FieldType.Unknown, FieldType.of("Quaternion"))
        assertEquals(FieldHint.None, FieldHint.of(""))
        assertEquals(GizmoSettings(), s.gizmo)   // older engines send no gizmo block
    }
}
