/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 */

import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function resolveServerCommand(): string {
    const cfg = vscode.workspace.getConfiguration('riu');
    const configured = (cfg.get<string>('executablePath') || '').trim();
    if (configured) {
        return configured;
    }
    return process.platform === 'win32' ? 'riu-lsp.exe' : 'riu-lsp';
}

export function activate(_context: vscode.ExtensionContext) {
    const command = resolveServerCommand();

    const serverOptions: ServerOptions = {
        run: { command, args: [] },
        debug: { command, args: [] },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'riu' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.ut'),
        },
    };

    client = new LanguageClient('riu', 'Riu Language Server', serverOptions, clientOptions);
    client.start();
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
