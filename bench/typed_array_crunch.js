// Typed array crunch benchmark: intensive numerical processing on
// Float64Array and Float32Array buffers.
// Combines an N-body gravitational physics kernel (Velocity Verlet) with a
// Cooley-Tukey Radix-2 FFT kernel on typed array views.

import { measure } from './harness.js';

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

// --- 1. N-Body Simulation on Float64Array ---------------------------------
function runNBody(numBodies, steps, dt) {
  const rng = makeLCG(12345);
  const pos = new Float64Array(numBodies * 3);
  const vel = new Float64Array(numBodies * 3);
  const acc = new Float64Array(numBodies * 3);
  const mass = new Float64Array(numBodies);

  for (let i = 0; i < numBodies; i++) {
    const idx = i * 3;
    pos[idx] = (rng() - 0.5) * 100.0;
    pos[idx + 1] = (rng() - 0.5) * 100.0;
    pos[idx + 2] = (rng() - 0.5) * 100.0;

    vel[idx] = (rng() - 0.5) * 2.0;
    vel[idx + 1] = (rng() - 0.5) * 2.0;
    vel[idx + 2] = (rng() - 0.5) * 2.0;

    mass[i] = 10.0 + rng() * 90.0;
  }

  const eps2 = 1.0; // softening factor squared
  const G = 6.674e-2;

  function computeForces() {
    for (let i = 0; i < numBodies * 3; i++) acc[i] = 0.0;

    for (let i = 0; i < numBodies; i++) {
      const i3 = i * 3;
      const xi = pos[i3];
      const yi = pos[i3 + 1];
      const zi = pos[i3 + 2];

      for (let j = i + 1; j < numBodies; j++) {
        const j3 = j * 3;
        const dx = pos[j3] - xi;
        const dy = pos[j3 + 1] - yi;
        const dz = pos[j3 + 2] - zi;

        const distSqr = dx * dx + dy * dy + dz * dz + eps2;
        const dist = Math.sqrt(distSqr);
        const invDist3 = G / (dist * distSqr);

        const fijX = dx * invDist3;
        const fijY = dy * invDist3;
        const fijZ = dz * invDist3;

        acc[i3] += fijX * mass[j];
        acc[i3 + 1] += fijY * mass[j];
        acc[i3 + 2] += fijZ * mass[j];

        acc[j3] -= fijX * mass[i];
        acc[j3 + 1] -= fijY * mass[i];
        acc[j3 + 2] -= fijZ * mass[i];
      }
    }
  }

  computeForces();

  for (let step = 0; step < steps; step++) {
    // Verlet step 1: update pos and half-step vel
    for (let i = 0; i < numBodies; i++) {
      const i3 = i * 3;
      vel[i3] += 0.5 * dt * acc[i3];
      vel[i3 + 1] += 0.5 * dt * acc[i3 + 1];
      vel[i3 + 2] += 0.5 * dt * acc[i3 + 2];

      pos[i3] += dt * vel[i3];
      pos[i3 + 1] += dt * vel[i3 + 1];
      pos[i3 + 2] += dt * vel[i3 + 2];
    }

    computeForces();

    // Verlet step 2: complete vel update
    for (let i = 0; i < numBodies; i++) {
      const i3 = i * 3;
      vel[i3] += 0.5 * dt * acc[i3];
      vel[i3 + 1] += 0.5 * dt * acc[i3 + 1];
      vel[i3 + 2] += 0.5 * dt * acc[i3 + 2];
    }
  }

  let kineticEnergy = 0.0;
  for (let i = 0; i < numBodies; i++) {
    const i3 = i * 3;
    const v2 = vel[i3] * vel[i3] + vel[i3 + 1] * vel[i3 + 1] + vel[i3 + 2] * vel[i3 + 2];
    kineticEnergy += 0.5 * mass[i] * v2;
  }

  return kineticEnergy;
}

// --- 2. Radix-2 Cooley-Tukey FFT on Float32Array ---------------------------
function fftRadix2(real, imag, n) {
  // Bit-reversal permutation
  let j = 0;
  for (let i = 0; i < n - 1; i++) {
    if (i < j) {
      const tempR = real[i];
      real[i] = real[j];
      real[j] = tempR;

      const tempI = imag[i];
      imag[i] = imag[j];
      imag[j] = tempI;
    }
    let k = n >> 1;
    while (k <= j) {
      j -= k;
      k >>= 1;
    }
    j += k;
  }

  // Cooley-Tukey butterfly computations
  for (let len = 2; len <= n; len <<= 1) {
    const half = len >> 1;
    const angle = (-2.0 * Math.PI) / len;
    const wStepR = Math.cos(angle);
    const wStepI = Math.sin(angle);

    for (let i = 0; i < n; i += len) {
      let wR = 1.0;
      let wI = 0.0;

      for (let k = 0; k < half; k++) {
        const uR = real[i + k];
        const uI = imag[i + k];

        const tR = wR * real[i + k + half] - wI * imag[i + k + half];
        const tI = wR * imag[i + k + half] + wI * real[i + k + half];

        real[i + k] = uR + tR;
        imag[i + k] = uI + tI;
        real[i + k + half] = uR - tR;
        imag[i + k + half] = uI - tI;

        const nextWR = wR * wStepR - wI * wStepI;
        const nextWI = wR * wStepI + wI * wStepR;
        wR = nextWR;
        wI = nextWI;
      }
    }
  }
}

function runFFTBench(size, passes) {
  const rng = makeLCG(67890);
  const real = new Float32Array(size);
  const imag = new Float32Array(size);

  let totalPower = 0.0;

  for (let pass = 0; pass < passes; pass++) {
    for (let i = 0; i < size; i++) {
      real[i] = Math.sin(i * 0.05 + pass * 0.1) + (rng() - 0.5) * 0.1;
      imag[i] = 0.0;
    }

    fftRadix2(real, imag, size);

    for (let i = 0; i < size; i++) {
      totalPower += real[i] * real[i] + imag[i] * imag[i];
    }
  }

  return totalPower;
}

function runTypedArrayCrunch() {
  const energy = runNBody(200, 60, 0.01);
  const power = runFFTBench(1024, 150);

  const checksum = Math.round(energy + power);
  return checksum;
}

console.log('typed_array_crunch checksum=' +
  measure('typed_array_crunch', () => runTypedArrayCrunch()));
