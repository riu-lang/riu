package org.yux.lang.ide.highlighter

import com.intellij.lexer.Lexer
import com.intellij.openapi.editor.colors.TextAttributesKey
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase
import com.intellij.psi.tree.IElementType
import org.yux.lang.ide.lexer.YuxLexer
import org.yux.lang.ide.lexer.YuxTokenTypes

/**
 * 基础语法高亮器：提供注释、字符串、代码点、数字、关键字的高亮。
 * 语义高亮（类/函数/字段/参数等）由 LSP semantic tokens 提供。
 * 在 markdown 代码块等内嵌场景下，LSP 无法工作，此时仅使用此基础高亮。
 */
class YuxSyntaxHighlighter : SyntaxHighlighterBase() {

    override fun getHighlightingLexer(): Lexer = YuxLexer()

    override fun getTokenHighlights(tokenType: IElementType): Array<TextAttributesKey> =
        when (tokenType) {
            YuxTokenTypes.LINE_COMMENT -> COMMENT
            YuxTokenTypes.STRING       -> STRING
            YuxTokenTypes.CODE_POINT   -> CODE_POINT
            YuxTokenTypes.NUMBER       -> NUMBER
            YuxTokenTypes.KEYWORD      -> KEYWORD
            else -> EMPTY
        }

    companion object {
        private val COMMENT    = arrayOf(YuxColors.LINE_COMMENT)
        private val STRING     = arrayOf(YuxColors.STRING)
        private val CODE_POINT = arrayOf(YuxColors.CODE_POINT)
        private val NUMBER     = arrayOf(YuxColors.NUMBER)
        private val KEYWORD    = arrayOf(YuxColors.KEYWORD)
        private val EMPTY      = emptyArray<TextAttributesKey>()
    }
}
