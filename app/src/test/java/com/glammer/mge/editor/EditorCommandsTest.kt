package com.glammer.mge.editor

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class EditorCommandsTest {
    @Test
    fun buildsProtocolLines() {
        assertEquals("select 4", EditorCommands.select(4))
        assertEquals("expand 2 0", EditorCommands.expand(2, false))
        assertEquals("create 0 Entity", EditorCommands.create())
        assertEquals("create 3 Entity", EditorCommands.create(3, "  "))
        assertEquals("rename 5 Big Gun", EditorCommands.rename(5, "Big\nGun "))
        assertEquals("reparent 5 0 -1", EditorCommands.reparent(5, 0))
        assertEquals("set 5 Transform2D position 1 2", EditorCommands.set(5, "Transform2D", "position", "1 2"))
        assertEquals("scrub 9 5 Transform2D rotation 45", EditorCommands.scrub(9, 5, "Transform2D", "rotation", "45"))
        assertEquals("add 5 SpriteRenderer", EditorCommands.addComponent(5, "SpriteRenderer"))
        assertEquals("remove 5 SpriteRenderer", EditorCommands.removeComponent(5, "SpriteRenderer"))
    }

    @Test
    fun fieldParts() {
        assertEquals(listOf("1", "-2.5"), FieldValues.split(" 1, -2.5 "))
        assertEquals("1 5", FieldValues.withPart("1 2", 2, 1, "5"))
        assertEquals("0 0 7 0", FieldValues.withPart("", 4, 2, "7"))
    }

    @Test
    fun floatsUseDotAndStayShort() {
        assertEquals("3", FieldValues.formatFloat(3f))
        assertEquals("-0.25", FieldValues.formatFloat(-0.25f))
        assertEquals("0.1", FieldValues.formatFloat(0.1f))
        assertEquals(1.5f, FieldValues.parseFloat("1,5"))
        assertNull(FieldValues.parseFloat("abc"))
        assertNull(FieldValues.parseFloat("NaN"))
    }

    @Test
    fun scrubSteps() {
        assertEquals("100", FieldValues.scrubbed(90f, 10f, FieldType.F32, FieldHint.Angle))
        assertEquals("1.2", FieldValues.scrubbed(1f, 10f, FieldType.F32, FieldHint.None))
        assertEquals("3", FieldValues.scrubbed(1f, 8f, FieldType.I16, FieldHint.None))
        assertEquals("0", FieldValues.scrubbed(1f, -40f, FieldType.U16, FieldHint.None))
        assertEquals("-9", FieldValues.scrubbed(1f, -40f, FieldType.I32, FieldHint.None))
    }

    @Test
    fun colors() {
        assertEquals(0x80FF8000.toInt(), FieldValues.colorArgb("#FF800080"))
        assertEquals(0xFF112233.toInt(), FieldValues.colorArgb("112233"))
        assertNull(FieldValues.colorArgb("#12"))
        assertNull(FieldValues.colorArgb("#GG0000"))
    }
}
