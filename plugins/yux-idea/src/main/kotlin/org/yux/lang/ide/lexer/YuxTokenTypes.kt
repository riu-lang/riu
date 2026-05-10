package org.yux.lang.ide.lexer

import com.intellij.psi.tree.IElementType
import org.yux.lang.ide.YuxLanguage

class YuxTokenType(name: String) : IElementType(name, YuxLanguage)

object YuxTokenTypes {
    @JvmField val LINE_COMMENT = YuxTokenType("YUX_LINE_COMMENT")
    @JvmField val STRING = YuxTokenType("YUX_STRING")
    @JvmField val CODE_POINT = YuxTokenType("YUX_CODE_POINT")
    @JvmField val NUMBER = YuxTokenType("YUX_NUMBER")
    @JvmField val KEYWORD = YuxTokenType("YUX_KEYWORD")
    @JvmField val METADATA = YuxTokenType("YUX_METADATA")
    @JvmField val IDENTIFIER = YuxTokenType("YUX_IDENTIFIER")
    @JvmField val SYMBOL = YuxTokenType("YUX_SYMBOL")

    val KEYWORDS = setOf(
        "break", "catch", "cval", "draft", "elif", "else", "enum", "extern", "false", "fn", "if",
        "loop", "match", "null", "ret", "struct", "true", "try", "use", "val", "var"
    )
}
