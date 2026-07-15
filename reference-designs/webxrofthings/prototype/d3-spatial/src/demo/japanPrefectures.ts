/**
 * The 47 prefectures of Japan — static lookup table for the UC5 geo-scene.
 *
 * THREE-free on purpose: imported by both the browser cell
 * (`liveGeoSceneCell.ts`) and the mock-join-server's simulated
 * `/api/v1/geo/japan-temps` feed, so it must not pull renderer deps.
 *
 * Fields:
 * - `code`     — JIS X 0401 prefecture code (1 = Hokkaido … 47 = Okinawa).
 * - `station`  — JMA AMeDAS station ID of the prefectural-capital station,
 *                for the Phase-2 live adapter (first 2 digits are the JMA
 *                area code; values verified against amedastable.json).
 * - `lat/lon`  — capital-city coordinates (decimal degrees) for the
 *                Open-Meteo fallback and any future projected layout.
 * - `gridX/gridY` — tile-grid ("pixel Japan") cell, x east-positive,
 *                y south-positive. Standard one-prefecture-one-square
 *                arrangement: Hokkaido top-right, Tōhoku column, Kantō
 *                block, Shikoku 2×2, Kyushu lower-left, Okinawa offset
 *                bottom-left. No two rows share a cell (unit-tested).
 * - `areaKm2`  — land area, for the optional `tileScale: 'area'` mode.
 * - `julyMeanC` — typical mid-July daily-mean temperature (°C) used as the
 *                baseline by the deterministic simulator. Coarse by design.
 */

export interface Prefecture {
  code: number;
  nameEn: string;
  nameJa: string;
  station: string;
  lat: number;
  lon: number;
  gridX: number;
  gridY: number;
  areaKm2: number;
  julyMeanC: number;
}

export const JAPAN_PREFECTURES: readonly Prefecture[] = [
  { code: 1,  nameEn: 'Hokkaido',  nameJa: '北海道', station: '14163', lat: 43.06, lon: 141.35, gridX: 11, gridY: 0,  areaKm2: 83424, julyMeanC: 21.1 },
  { code: 2,  nameEn: 'Aomori',    nameJa: '青森',   station: '31312', lat: 40.82, lon: 140.74, gridX: 11, gridY: 1,  areaKm2: 9646,  julyMeanC: 23.3 },
  { code: 3,  nameEn: 'Iwate',     nameJa: '岩手',   station: '33431', lat: 39.70, lon: 141.15, gridX: 11, gridY: 2,  areaKm2: 15275, julyMeanC: 22.9 },
  { code: 4,  nameEn: 'Miyagi',    nameJa: '宮城',   station: '34392', lat: 38.27, lon: 140.87, gridX: 11, gridY: 3,  areaKm2: 7282,  julyMeanC: 24.2 },
  { code: 5,  nameEn: 'Akita',     nameJa: '秋田',   station: '32402', lat: 39.72, lon: 140.10, gridX: 10, gridY: 2,  areaKm2: 11638, julyMeanC: 24.1 },
  { code: 6,  nameEn: 'Yamagata',  nameJa: '山形',   station: '35426', lat: 38.24, lon: 140.36, gridX: 10, gridY: 3,  areaKm2: 9323,  julyMeanC: 24.9 },
  { code: 7,  nameEn: 'Fukushima', nameJa: '福島',   station: '36127', lat: 37.75, lon: 140.47, gridX: 10, gridY: 4,  areaKm2: 13784, julyMeanC: 25.4 },
  { code: 8,  nameEn: 'Ibaraki',   nameJa: '茨城',   station: '40201', lat: 36.34, lon: 140.45, gridX: 11, gridY: 5,  areaKm2: 6097,  julyMeanC: 25.4 },
  { code: 9,  nameEn: 'Tochigi',   nameJa: '栃木',   station: '41277', lat: 36.55, lon: 139.88, gridX: 10, gridY: 5,  areaKm2: 6408,  julyMeanC: 25.4 },
  { code: 10, nameEn: 'Gunma',     nameJa: '群馬',   station: '42251', lat: 36.39, lon: 139.06, gridX: 9,  gridY: 5,  areaKm2: 6362,  julyMeanC: 26.2 },
  { code: 11, nameEn: 'Saitama',   nameJa: '埼玉',   station: '43241', lat: 35.86, lon: 139.65, gridX: 9,  gridY: 6,  areaKm2: 3798,  julyMeanC: 26.7 },
  { code: 12, nameEn: 'Chiba',     nameJa: '千葉',   station: '45212', lat: 35.60, lon: 140.12, gridX: 11, gridY: 6,  areaKm2: 5157,  julyMeanC: 26.4 },
  { code: 13, nameEn: 'Tokyo',     nameJa: '東京',   station: '44132', lat: 35.69, lon: 139.69, gridX: 10, gridY: 6,  areaKm2: 2194,  julyMeanC: 26.9 },
  { code: 14, nameEn: 'Kanagawa',  nameJa: '神奈川', station: '46106', lat: 35.44, lon: 139.64, gridX: 10, gridY: 7,  areaKm2: 2416,  julyMeanC: 26.6 },
  { code: 15, nameEn: 'Niigata',   nameJa: '新潟',   station: '54232', lat: 37.90, lon: 139.02, gridX: 9,  gridY: 4,  areaKm2: 12584, julyMeanC: 25.5 },
  { code: 16, nameEn: 'Toyama',    nameJa: '富山',   station: '55102', lat: 36.70, lon: 137.21, gridX: 7,  gridY: 5,  areaKm2: 4248,  julyMeanC: 26.1 },
  { code: 17, nameEn: 'Ishikawa',  nameJa: '石川',   station: '56227', lat: 36.59, lon: 136.63, gridX: 6,  gridY: 5,  areaKm2: 4186,  julyMeanC: 26.5 },
  { code: 18, nameEn: 'Fukui',     nameJa: '福井',   station: '57066', lat: 36.07, lon: 136.22, gridX: 6,  gridY: 6,  areaKm2: 4190,  julyMeanC: 26.6 },
  { code: 19, nameEn: 'Yamanashi', nameJa: '山梨',   station: '49142', lat: 35.66, lon: 138.57, gridX: 8,  gridY: 6,  areaKm2: 4465,  julyMeanC: 26.3 },
  { code: 20, nameEn: 'Nagano',    nameJa: '長野',   station: '48156', lat: 36.65, lon: 138.18, gridX: 8,  gridY: 5,  areaKm2: 13562, julyMeanC: 24.3 },
  { code: 21, nameEn: 'Gifu',      nameJa: '岐阜',   station: '52586', lat: 35.39, lon: 136.72, gridX: 7,  gridY: 6,  areaKm2: 10621, julyMeanC: 27.2 },
  { code: 22, nameEn: 'Shizuoka',  nameJa: '静岡',   station: '50331', lat: 34.98, lon: 138.38, gridX: 8,  gridY: 7,  areaKm2: 7777,  julyMeanC: 26.3 },
  { code: 23, nameEn: 'Aichi',     nameJa: '愛知',   station: '51106', lat: 35.18, lon: 136.91, gridX: 7,  gridY: 7,  areaKm2: 5172,  julyMeanC: 27.0 },
  { code: 24, nameEn: 'Mie',       nameJa: '三重',   station: '53133', lat: 34.73, lon: 136.51, gridX: 6,  gridY: 8,  areaKm2: 5774,  julyMeanC: 26.7 },
  { code: 25, nameEn: 'Shiga',     nameJa: '滋賀',   station: '60216', lat: 35.00, lon: 135.87, gridX: 6,  gridY: 7,  areaKm2: 4017,  julyMeanC: 26.5 },
  { code: 26, nameEn: 'Kyoto',     nameJa: '京都',   station: '61286', lat: 35.02, lon: 135.76, gridX: 5,  gridY: 7,  areaKm2: 4612,  julyMeanC: 27.3 },
  { code: 27, nameEn: 'Osaka',     nameJa: '大阪',   station: '62078', lat: 34.69, lon: 135.52, gridX: 4,  gridY: 8,  areaKm2: 1905,  julyMeanC: 27.9 },
  { code: 28, nameEn: 'Hyogo',     nameJa: '兵庫',   station: '63518', lat: 34.69, lon: 135.20, gridX: 4,  gridY: 7,  areaKm2: 8401,  julyMeanC: 27.5 },
  { code: 29, nameEn: 'Nara',      nameJa: '奈良',   station: '64036', lat: 34.69, lon: 135.83, gridX: 5,  gridY: 8,  areaKm2: 3691,  julyMeanC: 26.5 },
  { code: 30, nameEn: 'Wakayama',  nameJa: '和歌山', station: '65042', lat: 34.23, lon: 135.17, gridX: 6,  gridY: 9,  areaKm2: 4725,  julyMeanC: 26.9 },
  { code: 31, nameEn: 'Tottori',   nameJa: '鳥取',   station: '69122', lat: 35.50, lon: 134.24, gridX: 3,  gridY: 7,  areaKm2: 3507,  julyMeanC: 26.2 },
  { code: 32, nameEn: 'Shimane',   nameJa: '島根',   station: '68132', lat: 35.47, lon: 133.05, gridX: 2,  gridY: 7,  areaKm2: 6708,  julyMeanC: 25.8 },
  { code: 33, nameEn: 'Okayama',   nameJa: '岡山',   station: '66408', lat: 34.66, lon: 133.93, gridX: 3,  gridY: 8,  areaKm2: 7114,  julyMeanC: 27.2 },
  { code: 34, nameEn: 'Hiroshima', nameJa: '広島',   station: '67437', lat: 34.40, lon: 132.46, gridX: 2,  gridY: 8,  areaKm2: 8479,  julyMeanC: 27.1 },
  { code: 35, nameEn: 'Yamaguchi', nameJa: '山口',   station: '81286', lat: 34.19, lon: 131.47, gridX: 1,  gridY: 8,  areaKm2: 6112,  julyMeanC: 26.6 },
  { code: 36, nameEn: 'Tokushima', nameJa: '徳島',   station: '71106', lat: 34.07, lon: 134.56, gridX: 5,  gridY: 9,  areaKm2: 4147,  julyMeanC: 26.8 },
  { code: 37, nameEn: 'Kagawa',    nameJa: '香川',   station: '72086', lat: 34.34, lon: 134.04, gridX: 4,  gridY: 9,  areaKm2: 1877,  julyMeanC: 27.5 },
  { code: 38, nameEn: 'Ehime',     nameJa: '愛媛',   station: '73166', lat: 33.84, lon: 132.77, gridX: 4,  gridY: 10, areaKm2: 5676,  julyMeanC: 27.0 },
  { code: 39, nameEn: 'Kochi',     nameJa: '高知',   station: '74182', lat: 33.56, lon: 133.53, gridX: 5,  gridY: 10, areaKm2: 7104,  julyMeanC: 26.7 },
  { code: 40, nameEn: 'Fukuoka',   nameJa: '福岡',   station: '82182', lat: 33.61, lon: 130.42, gridX: 1,  gridY: 9,  areaKm2: 4986,  julyMeanC: 27.8 },
  { code: 41, nameEn: 'Saga',      nameJa: '佐賀',   station: '85142', lat: 33.25, lon: 130.30, gridX: 0,  gridY: 9,  areaKm2: 2441,  julyMeanC: 27.3 },
  { code: 42, nameEn: 'Nagasaki',  nameJa: '長崎',   station: '84496', lat: 32.74, lon: 129.87, gridX: 0,  gridY: 10, areaKm2: 4131,  julyMeanC: 27.1 },
  { code: 43, nameEn: 'Kumamoto',  nameJa: '熊本',   station: '86141', lat: 32.79, lon: 130.74, gridX: 1,  gridY: 10, areaKm2: 7409,  julyMeanC: 27.6 },
  { code: 44, nameEn: 'Oita',      nameJa: '大分',   station: '83216', lat: 33.24, lon: 131.61, gridX: 2,  gridY: 9,  areaKm2: 6341,  julyMeanC: 27.0 },
  { code: 45, nameEn: 'Miyazaki',  nameJa: '宮崎',   station: '87376', lat: 31.91, lon: 131.42, gridX: 2,  gridY: 10, areaKm2: 7735,  julyMeanC: 27.3 },
  { code: 46, nameEn: 'Kagoshima', nameJa: '鹿児島', station: '88317', lat: 31.56, lon: 130.56, gridX: 1,  gridY: 11, areaKm2: 9187,  julyMeanC: 28.1 },
  { code: 47, nameEn: 'Okinawa',   nameJa: '沖縄',   station: '91197', lat: 26.21, lon: 127.68, gridX: 0,  gridY: 12, areaKm2: 2281,  julyMeanC: 28.9 },
] as const;

/** Grid extents — handy for layout math. */
export const JAPAN_GRID_COLS = 12; // max gridX + 1
export const JAPAN_GRID_ROWS = 13; // max gridY + 1

/** One station reading, as served by /api/v1/geo/japan-temps. */
export interface PrefectureTemp {
  code: number;
  nameEn: string;
  nameJa: string;
  tempC: number | null;
  obsTime: string;
}

export interface JapanTempsPayload {
  updated: string;
  /** Which upstream produced this payload. */
  source: 'jma' | 'open-meteo' | 'simulated';
  stations: PrefectureTemp[];
}

/**
 * Deterministic simulated temperatures — July baseline per prefecture plus
 * a diurnal swing and a slow per-prefecture wobble, all derived from wall
 * clock so repeated polls drift believably and smoke baselines reproduce.
 * Shared by the mock server (authoritative) and available client-side.
 */
export function simulatedJapanTemps(nowMs: number): JapanTempsPayload {
  const now = new Date(nowMs);
  // JST hour-of-day as a fraction (UTC+9), for the diurnal cycle.
  const jstH = ((now.getUTCHours() + 9) % 24) + now.getUTCMinutes() / 60;
  // Peak around 14:00 JST, trough around 04:00; ±3.2 °C swing.
  const diurnal = 3.2 * Math.sin(((jstH - 8) / 24) * 2 * Math.PI);
  const stations = JAPAN_PREFECTURES.map((p) => {
    // Slow per-prefecture wobble (~35 min period, phase from the code) so
    // adjacent polls differ and no two prefectures move in lockstep.
    const wobble = 0.8 * Math.sin(nowMs / 2_100_000 + p.code * 1.7);
    return {
      code: p.code,
      nameEn: p.nameEn,
      nameJa: p.nameJa,
      tempC: Math.round((p.julyMeanC + diurnal + wobble) * 10) / 10,
      obsTime: now.toISOString(),
    };
  });
  return { updated: now.toISOString(), source: 'simulated', stations };
}
