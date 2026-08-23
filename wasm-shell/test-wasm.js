const fs = require('fs');
const path = require('path');

async function main() {
  const bytes = fs.readFileSync(path.join(__dirname, 'shell.wasm'));
  const { instance } = await WebAssembly.instantiate(bytes, { env: {} });
  const ex = instance.exports;
  ex.shell_init();
  console.log('shell_init OK');

  const writeStr = (off, s) => {
    const b = Buffer.from(s, 'utf8');
    const v = new Uint8Array(ex.memory.buffer);
    for (let i = 0; i < b.length; i++) v[off + i] = b[i];
    v[off + b.length] = 0;
  };
  const readStr = (off) => {
    const v = new Uint8Array(ex.memory.buffer);
    const b = [];
    let i = 0;
    while (v[off + i] !== 0) { b.push(v[off + i]); i++; }
    return Buffer.from(b).toString('utf8');
  };
  const exec = (cmd) => {
    const len = Buffer.byteLength(cmd, 'utf8');
    const p = ex.shell_alloc(len + 1);
    writeStr(p, cmd);
    const r = ex.shell_exec(p, len);
    ex.shell_free(p);
    if (r !== 0) throw new Error('exec failed: ' + r);
    return readStr(ex.shell_get_output());
  };

  const tests = [
    ['whoami', 'usr40k'],
    ['pwd', '/home/usr40k/workspace'],
    ['echo hello world', 'hello world'],
    ['echo $USER', 'usr40k'],
    ['echo $HOME', '/home/usr40k'],
    ['cat hello.txt', 'Hello, world!'],
    ['cat README.md', '# usr40k // cybercraft'],
    ['grep wasm hello.txt', 'Try: grep wasm hello.txt'],
    ['head -2 README.md', '# usr40k // cybercraft'],
    ['uname', 'WebAssembly'],
    ['neofetch', 'usr40k@dumsh'],
    ['help', 'Available commands:'],
    ['env', 'HOME=/home/usr40k'],
    ['export FOO=bar', ''],
    ['echo $FOO', 'bar'],
    ['cd /home/usr40k', ''],
    ['pwd', '/home/usr40k'],
    ['cd workspace', ''],
    ['pwd', '/home/usr40k/workspace'],
    ['mkdir testdir', ''],
    ['ls', 'testdir/'],
    ['touch testfile.txt', ''],
    ['ls', 'testfile.txt'],
    ['rm testfile.txt', ''],
    ['rm -r testdir', ''],
    ['history', 'whoami'],
    ['banner', 'a dumb little shell'],
    ['ls -la', 'README.md'],
    ['cat -n hello.txt', '1  Hello, world!'],
    ['echo -n no newline', 'no newline'],
    ['echo -e "line1\\nline2"', 'line1'],
    ['grep -i WASM hello.txt', 'WASM filesystem'],
    ['grep -n wasm hello.txt', '4:'],
    ['wc -l hello.txt', '4'],
    ['wc -w hello.txt', ''],
    ['head -n 1 README.md', '# usr40k // cybercraft'],
    ['tail -n 1 README.md', '- Matrix: @usr40k:usr40k.dev'],
    ['date -u', 'UTC'],
    ['uname -s', 'WebAssembly'],
    ['uname -r', '1.0.0'],
    ['uname -m', 'wasm32'],
    ['mkdir -p newdir', ''],
    ['ls', 'newdir/'],
    ['rm -rf newdir', ''],
    ['ls', ''],
    ['history -n 2', ''],
    ['nonexistent_cmd', 'command not found'],
  ];

  let pass = 0, fail = 0;
  for (const [cmd, exp] of tests) {
    try {
      const out = exec(cmd);
      const ok = exp === '' || out.includes(exp);
      if (ok) { pass++; console.log('  OK  ' + cmd); }
      else { fail++; console.log('  FAIL ' + cmd + ' -> got: ' + JSON.stringify(out.slice(0, 80))); }
    } catch (e) {
      fail++; console.log('  ERR  ' + cmd + ' -> ' + e.message);
    }
  }
  console.log('\n' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail > 0 ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });