// Shared benchmark runner helper for bench/run_benchmarks.sh and bench/bench.ps1.
//
// Subcommands:
//   time    <cmd_json> <runs> [self|wall]
//   profile <cmd_json>
//   json-argv <arg...>
//   record-pure <name> <desc> <st_aot_json> <btype> <baselines_file> <prof_aot_json>
//   record-render <name> <desc> <category> <status> <st_json> <btype>
//   report <jsonl_file> <do_prof_0_or_1>

const BENCH_LINE_RE = /^\[bench\] (\S+) ms=([0-9.]+)/gm;

function round2(x) {
  return Math.round(x * 100) / 100;
}

function median(sorted) {
  const m = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 1 ? sorted[m] : (sorted[m - 1] + sorted[m]) / 2;
}

function mean(vals) {
  return vals.reduce((a, b) => a + b, 0) / vals.length;
}

function stdev(vals) {
  if (vals.length <= 1) return 0;
  const mu = mean(vals);
  let sum = 0;
  for (const v of vals) sum += (v - mu) * (v - mu);
  return Math.sqrt(sum / (vals.length - 1));
}

function splitLines(s) {
  return (s || '').replace(/\r\n/g, '\n').replace(/\r/g, '\n').split('\n');
}

function cmdTime(cmdJson, runsStr, mode = 'self') {
  const cmd = JSON.parse(cmdJson);
  const runs = parseInt(runsStr, 10);

  function elapsed(p, wallMs) {
    if (mode === 'wall') return wallMs;
    BENCH_LINE_RE.lastIndex = 0;
    let sum = 0;
    let count = 0;
    let m;
    while ((m = BENCH_LINE_RE.exec(p.stderr || '')) !== null) {
      sum += parseFloat(m[2]);
      count++;
    }
    return count > 0 ? sum : null;
  }

  let warm;
  try {
    warm = Bronze.exec(cmd, { timeoutMs: 120000 });
    if (warm.status !== 0) {
      console.log(JSON.stringify({ error: `Warmup exit code ${warm.status}: ${warm.stderr.trim()}` }));
      return;
    }
  } catch (e) {
    console.log(JSON.stringify({ error: `Warmup failed: ${String(e)}` }));
    return;
  }

  const timings = [];
  let lastOut = warm.stdout || '';
  let lastErr = warm.stderr || '';
  for (let i = 0; i < runs; i++) {
    const t0 = performance.now();
    const p = Bronze.exec(cmd, { timeoutMs: 120000 });
    const t1 = performance.now();
    if (p.status !== 0) {
      console.log(JSON.stringify({ error: `Run exit code ${p.status}: ${p.stderr.trim()}` }));
      return;
    }
    const ms = elapsed(p, t1 - t0);
    if (ms === null) {
      console.log(JSON.stringify({
        error: 'no [bench] line on stderr: the fixture does not time itself through bench/harness.js',
      }));
      return;
    }
    timings.push(ms);
    lastOut = p.stdout || '';
    lastErr = p.stderr || '';
  }

  const sorted = timings.slice().sort((a, b) => a - b);
  const combined = [...splitLines(lastOut), ...splitLines(lastErr)]
    .map(l => l.trim())
    .filter(l => l.length > 0 && !l.startsWith('[bench]'));
  let checksum = '';
  for (let i = combined.length - 1; i >= 0; i--) {
    let line = combined[i];
    const ci = line.indexOf('[console]');
    if (ci >= 0) line = line.slice(ci + 9).trim();
    if (line.includes('checksum=') || line.startsWith('APP ') || line.startsWith('DRV ')) {
      checksum = line;
      break;
    }
  }
  if (!checksum && combined.length > 0) checksum = combined[combined.length - 1];

  console.log(JSON.stringify({
    median_ms: round2(median(sorted)),
    min_ms: round2(sorted[0]),
    max_ms: round2(sorted[sorted.length - 1]),
    mean_ms: round2(mean(sorted)),
    stdev_ms: round2(stdev(sorted)),
    measured: mode,
    timings: sorted.map(round2),
    output: checksum,
  }));
}

function cmdProfile(cmdJson) {
  try {
    const cmd = JSON.parse(cmdJson);
    const p = Bronze.exec(cmd, {
      timeoutMs: 120000,
      env: {
        BRONZE_PROFILE: '1',
        BRONZE_IC_LOG: '1',
        BRONZE_GC_LOG: '1',
        WSLENV: 'BRONZE_PROFILE:BRONZE_IC_LOG:BRONZE_GC_LOG',
      },
    });
    const output = (p.stdout || '') + '\n' + (p.stderr || '');
    const lines = splitLines(output);

    const topHelpers = [];
    let totalHelpers = 0;
    let inProfile = false;
    for (const raw of lines) {
      const s = raw.trim();
      if (s.includes('Total Dynamic ABI Helper Invocations:')) {
        const parts = s.split(':');
        if (parts.length > 1) {
          const n = parseInt(parts[1].trim(), 10);
          if (!Number.isNaN(n)) totalHelpers = n;
        }
      } else if (s.includes('=== Bronze Runtime Profile')) {
        inProfile = true;
      } else if (inProfile && s.startsWith('---')) {
        continue;
      } else if (inProfile && (s.includes('=== Bronze GC Log') || s.startsWith('==='))) {
        inProfile = false;
      } else if (inProfile && s) {
        const parts = s.split(/\s+/);
        if (parts.length >= 2 && parts[0].startsWith('bronze_')) {
          const n = parseInt(parts[1], 10);
          if (!Number.isNaN(n)) topHelpers.push({ name: parts[0], count: n });
        }
      }
    }

    let topMiss = '';
    let inMiss = false;
    for (const raw of lines) {
      const s = raw.trim();
      if (s.includes('--- bronze_prop_get Misses by Reason') || s.includes('--- bronze_prop_set Misses by Reason')) {
        inMiss = true;
      } else if (inMiss && s.startsWith('---')) {
        continue;
      } else if (inMiss && s.startsWith('===')) {
        inMiss = false;
      } else if (inMiss && s && !topMiss) {
        const parts = s.split(/\s+/);
        if (parts.length >= 2) {
          topMiss = `${parts[0]} (${parts[1]})`;
          inMiss = false;
        }
      }
    }

    console.log(JSON.stringify({
      total_helpers: totalHelpers,
      top_helpers: topHelpers.slice(0, 3),
      top_miss: topMiss,
    }));
  } catch (e) {
    console.log(JSON.stringify({ error: String(e) }));
  }
}

function cmdRecordPure(name, desc, stAotStr, btype, baselinesFile, profAotStr) {
  const stAot = stAotStr ? JSON.parse(stAotStr) : {};
  const profAot = profAotStr ? JSON.parse(profAotStr) : {};

  let nodeMed = null;
  if (baselinesFile && Bronze.fs.exists(baselinesFile)) {
    try {
      const bdata = JSON.parse(Bronze.fs.readText(baselinesFile));
      const entry = bdata && bdata.benchmarks && bdata.benchmarks[name];
      if (entry && entry.median_ms !== undefined) nodeMed = entry.median_ms;
    } catch (_) {}
  }

  if (stAot.error) {
    console.log(JSON.stringify({ name, category: 'pure-compute', build_type: btype, error: String(stAot.error) }));
    return;
  }

  const aotMed = stAot.median_ms;
  const vsNode = (nodeMed && aotMed && aotMed > 0) ? round2(nodeMed / aotMed) : null;

  let status = 'N/A';
  if (vsNode !== null) {
    if (vsNode >= 1.05) status = 'WIN';
    else if (vsNode >= 0.95) status = 'PARITY';
    else status = 'BEHIND';
  }

  const hasAot = Object.keys(stAot).length > 0;
  const hasProf = Object.keys(profAot).length > 0;
  console.log(JSON.stringify({
    name,
    description: desc,
    category: 'pure-compute',
    build_type: btype,
    aot: hasAot ? stAot : null,
    node_median_ms: nodeMed,
    vs_node: vsNode,
    status,
    profile: hasProf ? profAot : null,
  }));
}

function cmdRecordRender(name, desc, category, status, stStr, btype) {
  const st = stStr ? JSON.parse(stStr) : {};
  console.log(JSON.stringify({
    name,
    description: desc,
    category,
    build_type: btype,
    aot: st,
    node_median_ms: null,
    vs_node: null,
    status,
  }));
}

function fmtInt(n) {
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function cmdReport(jsonlFile, doProfStr) {
  const doProf = parseInt(doProfStr, 10) !== 0;
  const records = [];
  try {
    for (const line of splitLines(Bronze.fs.readText(jsonlFile))) {
      const t = line.trim();
      if (t) records.push(JSON.parse(t));
    }
  } catch (e) {
    console.log(`Failed to read results: ${String(e)}`);
    return;
  }
  if (records.length === 0) {
    console.log('No benchmark results generated.');
    return;
  }

  console.log('\n### Pure-Compute Benchmarks (Compiled Native vs Node.js Baseline)');
  console.log('| Benchmark | Bronze AOT (ms) | Node.js (ms) | vs Node | Status | Checksum / Output |');
  console.log('|---|---|---|---|---|---|');

  for (const r of records) {
    if (r.category !== 'pure-compute') continue;
    const name = r.name || '';
    if (r.error) {
      console.log(`| \`${name}\` | ERROR | - | - | ERROR | ${r.error} |`);
      continue;
    }
    const aotSt = r.aot || r.infer || null;
    const aotMed = aotSt ? Number(aotSt.median_ms || 0).toFixed(2) : '-';
    const nodeMed = r.node_median_ms != null ? Number(r.node_median_ms).toFixed(2) : '-';
    const vsNode = r.vs_node != null ? `${Number(r.vs_node).toFixed(2)}x` : '-';
    const status = r.status || '-';
    const badge = status === 'WIN' ? '**WIN**' : (status === 'PARITY' || status === 'BEHIND' ? status : '-');
    const out = aotSt ? (aotSt.output || '') : '';
    console.log(`| \`${name}\` | **${aotMed}** | ${nodeMed} | **${vsNode}** | ${badge} | \`${out}\` |`);
  }

  if (doProf) {
    console.log('\n#### Profile Miss Breakdown (--profile)');
    console.log('| Benchmark | Total Helpers | Top ABI Helpers | Top IC Miss Reason |');
    console.log('|---|---|---|---|');
    for (const r of records) {
      if (r.category !== 'pure-compute' || !r.profile) continue;
      const tot = r.profile.total_helpers || 0;
      const helpers = (r.profile.top_helpers || []).map(h => `${h.name}: ${h.count}`).join(', ') || 'None';
      const topMiss = r.profile.top_miss || 'None';
      console.log(`| \`${r.name || ''}\` | ${fmtInt(tot)} | ${helpers} | ${topMiss} |`);
    }
  }

  const renderRecords = records.filter(r => r.category !== 'pure-compute');
  if (renderRecords.length > 0) {
    console.log('\n### Render Scene Benchmarks (Bro Host Execution)');
    console.log('| Scene Benchmark | Mode / Runtime | Median (ms) | Min (ms) | Max (ms) | Checksum / Status |');
    console.log('|---|---|---|---|---|---|');
    for (const r of renderRecords) {
      const name = r.name || '';
      if (r.error) {
        console.log(`| \`${name}\` | ${r.category || ''} | ERROR | - | - | ${r.error} |`);
        continue;
      }
      const st = r.aot || r.infer || {};
      const cat = (r.category || '').includes('compiled') ? 'Compiled Host' : 'bro-headless (in-process bronze)';
      console.log(
        `| \`${name}\` | ${cat} | **${Number(st.median_ms || 0).toFixed(2)}** | ` +
        `${Number(st.min_ms || 0).toFixed(2)} | ${Number(st.max_ms || 0).toFixed(2)} | \`${st.output || ''}\` |`
      );
    }
  }

  console.log(`\nMachine-readable results saved to \`${jsonlFile}\`.`);
}

const [sub, ...rest] = Bronze.args;
if (sub === 'time') cmdTime(rest[0], rest[1], rest[2] || 'self');
else if (sub === 'profile') cmdProfile(rest[0]);
else if (sub === 'json-argv') console.log(JSON.stringify(rest));
else if (sub === 'record-pure') cmdRecordPure(rest[0], rest[1], rest[2], rest[3], rest[4], rest[5]);
else if (sub === 'record-render') cmdRecordRender(rest[0], rest[1], rest[2], rest[3], rest[4], rest[5]);
else if (sub === 'report') cmdReport(rest[0], rest[1] || '0');
else {
  console.error('unknown bench_helper subcommand: ' + (sub || ''));
  Bronze.exit(2);
}
