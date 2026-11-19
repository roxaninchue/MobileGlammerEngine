package com.glammer.mge.editor

import org.junit.Assert.assertEquals
import org.junit.Test

class FieldPartsTest {
    @Test
    fun threeDimensionalFieldsEditAsThreeParts() {
        // As the engine sends them for Transform3D (M3 #26).
        val json = """{"rev":1,"insp":{"id":1,"name":"Box","components":[{"type":"Transform3D","removable":false,"fields":[
            {"name":"position","type":"Vec3","hint":"","value":"1 2 3"},
            {"name":"rotation","type":"Vec4","hint":"Rotation","value":"0 90 0"},
            {"name":"scale","type":"Vec3","hint":"","value":"1 1 1"}]}],"addable":[]}}"""
        val f = EditorState.parse(json).inspector!!.components[0].fields
        assertEquals(listOf(FieldType.Vec3, FieldType.Vec4, FieldType.Vec3), f.map { it.type })
        assertEquals(FieldHint.Rotation, f[1].hint)
        assertEquals(listOf(3, 3, 3), f.map { it.parts })
        assertEquals("1 5 3", FieldValues.withPart(f[0].value, 3, 1, "5"))
    }
}
