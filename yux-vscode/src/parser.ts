/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * antlr4ng + 生成器产物的薄包装：给一段 yux 源码，返回 parse tree。
 * 真正的遍历/符号抽取见 `ast/visitor.ts`（P1 加入）。
 */

import { CharStream, CommonTokenStream, BaseErrorListener, RecognitionException, Recognizer, Token, ATNSimulator } from 'antlr4ng';
import { yuxLexer } from './gen/yuxLexer';
import { yuxParser, ProgramContext } from './gen/yuxParser';

export interface YuxSyntaxError {
    line: number;       // 1-based
    column: number;     // 0-based
    message: string;
    offendingSymbol: string | null;
}

export interface YuxParseResult {
    tree: ProgramContext;
    errors: YuxSyntaxError[];
    tokens: CommonTokenStream;
}

class CollectingErrorListener extends BaseErrorListener {
    readonly errors: YuxSyntaxError[] = [];

    override syntaxError<S extends Token, T extends ATNSimulator>(
        _recognizer: Recognizer<T>,
        offendingSymbol: S | null,
        line: number,
        column: number,
        message: string,
        _e: RecognitionException | null,
    ): void {
        this.errors.push({
            line,
            column,
            message,
            offendingSymbol: offendingSymbol?.text ?? null,
        });
    }
}

export function parseYux(source: string): YuxParseResult {
    const input = CharStream.fromString(source);
    const lexer = new yuxLexer(input);
    lexer.removeErrorListeners();
    const lexerListener = new CollectingErrorListener();
    lexer.addErrorListener(lexerListener);

    const tokens = new CommonTokenStream(lexer);
    const parser = new yuxParser(tokens);
    parser.removeErrorListeners();
    const parserListener = new CollectingErrorListener();
    parser.addErrorListener(parserListener);

    const tree = parser.program();

    return {
        tree,
        errors: [...lexerListener.errors, ...parserListener.errors],
        tokens,
    };
}
