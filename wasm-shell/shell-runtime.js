/**
 * usr40k web-shell runtime
 *
 * Loads the compiled WASM shell module, provides the JS<->WASM bridge,
 * and exposes a simple API for the terminal UI to execute commands.
 */
(function (global) {
  'use strict';

  const SHELL_WASM_URL = 'wasm-shell/shell.wasm';

  class WasmShell {
    constructor() {
      this.instance = null;
      this.memory = null;
      this.ready = false;
      this.initPromise = null;
    }

    /**
     * Initialize the WASM module. Returns a promise that resolves
     * once the shell is ready to accept commands.
     */
    async init() {
      if (this.ready) return;
      if (this.initPromise) return this.initPromise;

      this.initPromise = (async () => {
        const response = await fetch(SHELL_WASM_URL);
        if (!response.ok) {
          throw new Error(`Failed to load ${SHELL_WASM_URL}: ${response.status} ${response.statusText}`);
        }
        const bytes = await response.arrayBuffer();

        const importObject = {
          env: {
            // Fallback imports in case the compiler emits calls to libc
            // builtins (e.g. strlen). The shell is otherwise self-contained.
            strlen: (ptr) => {
              const view = new Uint8Array(this.memory.buffer);
              let len = 0;
              while (view[ptr + len] !== 0) len++;
              return len;
            },
            memcpy: (dst, src, n) => {
              const view = new Uint8Array(this.memory.buffer);
              view.copyWithin(dst, src, src + n);
              return dst;
            },
            memset: (ptr, val, n) => {
              const view = new Uint8Array(this.memory.buffer);
              view.fill(val, ptr, ptr + n);
              return ptr;
            },
            memmove: (dst, src, n) => {
              const view = new Uint8Array(this.memory.buffer);
              view.copyWithin(dst, src, src + n);
              return dst;
            }
          }
        };

        const { instance } = await WebAssembly.instantiate(bytes, importObject);
        this.instance = instance;
        this.memory = instance.exports.memory;

        // Initialize the shell (filesystem, env, etc.)
        instance.exports.shell_init();

        this.ready = true;
        return this;
      })();

      return this.initPromise;
    }

    /**
     * Write a JS string into WASM linear memory at the given offset.
     * Returns the number of bytes written (including null terminator).
     */
    writeString(offset, str) {
      const bytes = new TextEncoder().encode(str);
      const view = new Uint8Array(this.memory.buffer);
      for (let i = 0; i < bytes.length; i++) {
        view[offset + i] = bytes[i];
      }
      view[offset + bytes.length] = 0;
      return bytes.length + 1;
    }

    /**
     * Read a null-terminated string from WASM linear memory.
     */
    readString(offset) {
      const view = new Uint8Array(this.memory.buffer);
      const bytes = [];
      let i = 0;
      while (true) {
        const b = view[offset + i];
        if (b === 0) break;
        bytes.push(b);
        i++;
      }
      return new TextDecoder().decode(new Uint8Array(bytes));
    }

    /**
     * Execute a shell command. Returns the command's output as a string.
     */
    exec(command) {
      if (!this.ready) {
        throw new Error('Shell not initialized. Call init() first.');
      }

      const exports = this.instance.exports;

      // Allocate memory for the command string
      const cmdLen = new TextEncoder().encode(command).length;
      const cmdPtr = exports.shell_alloc(cmdLen + 1);
      if (!cmdPtr) {
        throw new Error('Failed to allocate memory for command');
      }

      // Write the command into WASM memory
      this.writeString(cmdPtr, command);

      // Execute
      const result = exports.shell_exec(cmdPtr, cmdLen);

      // Free the command buffer
      exports.shell_free(cmdPtr);

      if (result !== 0) {
        throw new Error(`Shell execution failed with code ${result}`);
      }

      // Read the output
      const outPtr = exports.shell_get_output();
      const outLen = exports.shell_get_output_len();
      const output = this.readString(outPtr);

      return output;
    }

    /**
     * Check if the shell is ready.
     */
    isReady() {
      return this.ready;
    }
  }

  // Singleton instance
  const shell = new WasmShell();

  // Expose globally
  global.WasmShell = WasmShell;
  global.wasmShell = shell;

})(typeof window !== 'undefined' ? window : this);