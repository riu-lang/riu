package org.yux.lang.ide.parser

import com.intellij.lang.ASTNode
import com.intellij.lang.ParserDefinition
import com.intellij.lang.PsiParser
import com.intellij.lexer.Lexer
import com.intellij.openapi.project.Project
import com.intellij.psi.FileViewProvider
import com.intellij.psi.PsiElement
import com.intellij.psi.PsiFile
import com.intellij.psi.tree.IFileElementType
import com.intellij.psi.tree.TokenSet
import org.yux.lang.ide.YuxLanguage
import org.yux.lang.ide.lexer.YuxLexer
import org.yux.lang.ide.lexer.YuxTokenTypes
import org.yux.lang.ide.psi.YuxFile

class YuxParserDefinition : ParserDefinition {

    override fun createLexer(project: Project?): Lexer = YuxLexer()

    override fun createParser(project: Project?): PsiParser = YuxFlatParser()

    override fun getFileNodeType(): IFileElementType = FILE

    override fun getCommentTokens(): TokenSet = COMMENTS

    override fun getStringLiteralElements(): TokenSet = STRINGS

    override fun createElement(node: ASTNode): PsiElement =
        com.intellij.psi.impl.source.tree.LeafPsiElement(node.elementType, node.text)

    override fun createFile(viewProvider: FileViewProvider): PsiFile = YuxFile(viewProvider)

    companion object {
        val FILE = IFileElementType(YuxLanguage)
        val COMMENTS: TokenSet = TokenSet.create(YuxTokenTypes.LINE_COMMENT)
        val STRINGS: TokenSet = TokenSet.create(YuxTokenTypes.STRING, YuxTokenTypes.CODE_POINT)
    }
}
