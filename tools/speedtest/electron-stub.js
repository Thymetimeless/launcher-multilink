// Plexo's testKnobs.ts only reads app.isPackaged; true is what a released Plexo build sees,
// so no test overrides (PLEXO_E2E_*) can leak into the speed test.
export const app = { isPackaged: true }
