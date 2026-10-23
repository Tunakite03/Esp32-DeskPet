// Execute the actual inline setup script with a minimal DOM and network stub.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../src/features/setup/setup_portal.cpp'), 'utf8');
const script = source.split('html += R"HTML(<script>')[1].split('</script></body></html>)HTML";')[0];
assert.ok(script.includes('function noteSetupActivity()'));

async function main() {
  let now = 0;
  let fetchMode = "ok";
  let responseOk = true;
  const requests = [];
  const timers = new Map();
  let timerId = 0;
  const settle = () => new Promise(resolve => setImmediate(resolve));
  const listeners = {};
  const element = () => ({addEventListener() {}, style: {}});
  const elements = new Map();
  const document = {
    getElementById(id) {
      if (id === 'scan_opt') return null;
      if (!elements.has(id)) elements.set(id, element());
      return elements.get(id);
    },
    querySelector(selector) {
      if (selector === 'input[name="token"]') return {value: 'test+token&value'};
      assert.equal(selector, 'form');
      return element();
    },
    addEventListener(type, callback) { listeners[type] = callback; }
  };
  vm.runInNewContext(script, {
    document,
    performance: {now: () => now},
    AbortController,
    fetch(url, options) {
      requests.push({url, options});
      if (fetchMode === 'fail') return Promise.reject(new Error('offline'));
      if (fetchMode === 'hang') return new Promise((resolve, reject) => {
        options.signal.addEventListener('abort', () => reject(new Error('aborted')));
      });
      return Promise.resolve({ok: responseOk});
    },
    setTimeout(fn, delay) { timers.set(++timerId, {fn, delay}); return timerId; },
    clearTimeout(id) { timers.delete(id); },
    Date
  });
  assert.equal(requests.length, 0, 'An idle form must not send heartbeat requests');
  assert.equal(timers.size, 0, 'An idle form must not schedule a heartbeat');
  for (const type of ['input', 'change', 'pointerdown', 'keydown']) assert.equal(typeof listeners[type], 'function');
  listeners.input();
  await settle();
  assert.equal(requests.length, 1);
  assert.equal(requests[0].url, '/activity');
  assert.equal(requests[0].options.method, 'POST');
  assert.equal(requests[0].options.body, 'token=test%2Btoken%26value');
  assert.equal(requests[0].options.headers['Content-Type'], 'application/x-www-form-urlencoded');
  for (now = 1; now < 15000; now += 100) listeners.keydown();
  assert.equal(requests.length, 1, 'Typing must be throttled, not send per key');
  now = 15000;
  listeners.change();
  await settle();
  assert.equal(requests.length, 2);
  now = 30000;
  fetchMode = 'fail';
  listeners.pointerdown();
  await settle();
  assert.equal(requests.length, 3, 'Network failure must not break editing');
  now = 30001;
  listeners.input();
  assert.equal(requests.length, 3, 'Failed requests must still have a short rate limit');
  now = 31000;
  fetchMode = 'ok';
  listeners.input();
  await settle();
  assert.equal(requests.length, 4, 'Active editing retries a network failure after 1 second');
  now = 46000;
  responseOk = false;
  listeners.input();
  await settle();
  assert.equal(requests.length, 5);
  now = 47000;
  responseOk = true;
  listeners.input();
  await settle();
  assert.equal(requests.length, 6, 'A rejected HTTP response must not earn the long throttle');
  now = 62000;
  fetchMode = 'hang';
  listeners.input();
  listeners.keydown();
  assert.equal(requests.length, 7, 'Do not overlap in-flight requests');
  assert.equal(timers.size, 1);
  const timeout = [...timers.values()][0];
  assert.equal(timeout.delay, 2000);
  now = 64000;
  timeout.fn();
  await settle();
  assert.equal(timers.size, 0, 'Abort and clean up a stalled keepalive');
  fetchMode = 'ok';
  listeners.input();
  await settle();
  assert.equal(requests.length, 8, 'User input can retry after a stalled request');
  now = 600000;
  assert.equal(requests.length, 8, 'An abandoned connected tab must not extend setup');
  assert.equal(timers.size, 0, 'No retry timer or heartbeat survives the request');
  console.log('PASS: activity token, success throttling, no idle heartbeat, network/HTTP retry, stalled request abort');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
