package org.yux.lang.ide.highlighter

import com.intellij.openapi.editor.DefaultLanguageHighlighterColors as Default
import com.intellij.openapi.editor.colors.TextAttributesKey

/**
 * yux 语言所有可在 Settings → Editor → Color Scheme → Yux 中调整的颜色键。
 * fallback 指向 IntelliJ 通用类别，主题切换时自动跟随。
 */
object YuxColors {
    val LINE_COMMENT  = key("YUX_LINE_COMMENT", Default.LINE_COMMENT)
    val STRING        = key("YUX_STRING",       Default.STRING)
    val CODE_POINT    = key("YUX_CODE_POINT",   Default.STRING)
    val NUMBER        = key("YUX_NUMBER",       Default.NUMBER)
    val KEYWORD       = key("YUX_KEYWORD",      Default.KEYWORD)
    val OPERATOR      = key("YUX_OPERATOR",     Default.OPERATION_SIGN)

    val IDENTIFIER    = key("YUX_IDENTIFIER",   Default.IDENTIFIER)
    val VARIABLE      = key("YUX_VARIABLE",     Default.LOCAL_VARIABLE)
    val PARAMETER     = key("YUX_PARAMETER",    Default.PARAMETER)
    val PROPERTY      = key("YUX_PROPERTY",     Default.INSTANCE_FIELD)

    val CLASS                = key("YUX_CLASS",                Default.CLASS_NAME)
    val FUNCTION_DECLARATION = key("YUX_FUNCTION_DECLARATION", Default.FUNCTION_DECLARATION)
    val FUNCTION_CALL        = key("YUX_FUNCTION_CALL",        Default.FUNCTION_CALL)
    val METHOD               = key("YUX_METHOD",               Default.INSTANCE_METHOD)

    private fun key(name: String, fallback: TextAttributesKey): TextAttributesKey =
        TextAttributesKey.createTextAttributesKey(name, fallback)
}
