const nsfw = require('../src/index');
const { performance } = require('perf_hooks');

function withTimeout(promise, timeoutMS, label) {
  if (!Number.isFinite(timeoutMS) || timeoutMS <= 0) {
    return promise;
  }

  return Promise.race([
    promise,
    new Promise((_, reject) => {
      setTimeout(() => {
        reject(new Error(`${label}_timeout_ms=${timeoutMS}`));
      }, timeoutMS);
    }),
  ]);
}

async function main() {
  const targetPath = process.argv[2];

  if (!targetPath) {
    throw new Error('Usage: node js/scripts/benchmark-start.js <absolute-path> [debounceMS] [settleMS]');
  }

  const debounceMS = Number.parseInt(process.argv[3] || '500', 10);
  const settleMS = Number.parseInt(process.argv[4] || '1000', 10);
  const startTimeoutMS = Number.parseInt(process.argv[5] || '600000', 10);
  const stopTimeoutMS = Number.parseInt(process.argv[6] || '60000', 10);

  console.log('phase=build');

  const startWall = performance.now();
  const watcher = await nsfw(targetPath, () => {}, {
    debounceMS,
    errorCallback(error) {
      console.error('watcher-error:', error && error.message ? error.message : error);
    },
    excludedPaths: [],
  });

  const startCall = performance.now();
  console.log('phase=start');
  await withTimeout(watcher.start(), startTimeoutMS, 'start');
  const startedAt = performance.now();

  console.log('phase=active');
  await new Promise((resolve) => setTimeout(resolve, settleMS));

  const beforeStop = performance.now();
  console.log('phase=stop');
  await withTimeout(watcher.stop(), stopTimeoutMS, 'stop');
  const endWall = performance.now();

  if (global.gc) {
    global.gc();
  }

  const mem = process.memoryUsage();

  console.log(`target=${targetPath}`);
  console.log(`debounce_ms=${debounceMS}`);
  console.log(`settle_ms=${settleMS}`);
  console.log(`start_timeout_ms=${startTimeoutMS}`);
  console.log(`stop_timeout_ms=${stopTimeoutMS}`);
  console.log(`construct_ms=${(startCall - startWall).toFixed(2)}`);
  console.log(`start_ms=${(startedAt - startCall).toFixed(2)}`);
  console.log(`active_ms=${(beforeStop - startedAt).toFixed(2)}`);
  console.log(`total_ms=${(endWall - startWall).toFixed(2)}`);
  console.log(`node_rss_bytes=${mem.rss}`);
  console.log(`node_heap_used_bytes=${mem.heapUsed}`);
  process.exit(0);
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
