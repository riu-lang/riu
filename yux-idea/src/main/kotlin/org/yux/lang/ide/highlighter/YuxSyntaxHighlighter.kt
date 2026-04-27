package org.yux.lang.ide.highlighter

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.tree.IElementType
import org.yux.lang.ide.lexer.YuxLexer
import org.yux.lang.ide.lexer.YuxTokenTypes

/**
 * 仅给 LSP 未就绪/断线时的兜底色。语义色（关键字/类/函数/字段/参数）由 LSP semantic tokens 决定。
 */
class YuxSyntaxHighlighter : SyntaxHighlighterBase() {

    override fun getHighlightingLexer(): Lexer = YuxLexer()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> =
        when (tokenType) {
            YuxTokenTypes.LINE_COMMENT -> COMMENT
            YuxTokenTypes.STRING       -> STRING
            YuxTokenTypes.CODE_POINT   -> CODE_POINT
            YuxTokenTypes.NUMBER       -> NUMBER
            else -> EMPTY
        }

    companion object {
        private val COMMENT    = arrayOf(YuxColors.LINE_COMMENT)
        private val STRING     = arrayOf(YuxColors.STRING)
        private val CODE_POINT = arrayOf(YuxColors.CODE_POINT)
        private val NUMBER     = arrayOf(YuxColors.NUMBER)
        private val EMPTY      = emptyArray<TextAttributesKey>()
    }
}
