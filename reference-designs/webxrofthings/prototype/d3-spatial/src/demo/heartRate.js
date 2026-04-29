import { scaleLinear, scaleTime } from 'd3-scale';
import { extent } from 'd3-array';
import { Chart } from '../chart/Chart';
export function syntheticHR(minutes = 60, samplesPerMin = 4, seed = 42) {
    const n = minutes * samplesPerMin;
    const now = Date.now();
    const rand = mulberry32(seed);
    const out = [];
    let v = 72;
    for (let i = 0; i < n; i++) {
        const t = now - (n - i) * (60_000 / samplesPerMin);
        const drift = Math.sin(i / 18) * 8 + Math.sin(i / 5) * 3;
        v = 72 + drift + (rand() - 0.5) * 6;
        out.push({ t, v });
    }
    return out;
}
export function buildHeartRateChart() {
    const data = syntheticHR();
    const tDomain = extent(data, d => d.t);
    const vDomain = [
        Math.min(50, Math.min(...data.map(d => d.v)) - 5),
        Math.max(110, Math.max(...data.map(d => d.v)) + 5),
    ];
    const x = scaleTime().domain(tDomain).range([0, 0.4]);
    const y = scaleLinear().domain(vDomain).range([0, 0.1]);
    const chart = new Chart({
        xAccessor: (d) => d.t,
        yAccessor: (d) => d.v,
        markOptions: { color: 0xff5577, radius: 0.003 },
    })
        .x(x)
        .y(y)
        .mark('line')
        .data(data)
        .render();
    return chart;
}
function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
