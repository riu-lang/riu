// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "parse_program.h"

#include "BailErrorStrategy.h"
#include "DefaultErrorStrategy.h"
#include "Exceptions.h"
#include "RecognitionException.h"
#include "atn/ParserATNSimulator.h"
#include "atn/PredictionMode.h"

yux::yuxParser::ProgramContext* parseYuxProgram(yux::yuxParser& parser, antlr4::CommonTokenStream& tokens,
                                                antlr4::ANTLRErrorListener* errListener) {
    auto* interp = parser.getInterpreter<antlr4::atn::ParserATNSimulator>();
    parser.removeErrorListeners();
    parser.setErrorHandler(std::make_shared<antlr4::BailErrorStrategy>());
    interp->setPredictionMode(antlr4::atn::PredictionMode::SLL);
    try {
        return parser.program();
    } catch (const antlr4::ParseCancellationException&) { // NOLINT(bugprone-empty-catch)
    } catch (const antlr4::RecognitionException&) {       // NOLINT(bugprone-empty-catch)
    }

    tokens.seek(0);
    parser.reset();
    if (errListener) parser.addErrorListener(errListener);
    parser.setErrorHandler(std::make_shared<antlr4::DefaultErrorStrategy>());
    interp = parser.getInterpreter<antlr4::atn::ParserATNSimulator>();
    interp->setPredictionMode(antlr4::atn::PredictionMode::LL);
    return parser.program();
}
