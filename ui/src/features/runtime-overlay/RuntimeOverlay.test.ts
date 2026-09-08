import { describe, expect, test } from "bun:test";
import type { RuntimeOverlaySnapshot } from "../../bridge/contracts";
import {
  mergeRuntimeOverlaySnapshot,
  overlayColor,
} from "./runtimeOverlayModel";

describe("runtime overlay color conversion", () => {
  test("normalizes configured colors to the monochrome app theme", () => {
    expect(overlayColor(0xff0000ff)).toBe("var(--color-ink)");
    expect(overlayColor(0x8000ff00)).toBe("var(--color-ink)");
  });
});

describe("runtime overlay focus stream", () => {
  const snapshot = (
    sequence: string,
    focusRevision: string,
    resumeRequired: boolean,
  ) => ({ sequence, focusRevision, resumeRequired }) as RuntimeOverlaySnapshot;
  test("focus-only transitions update the prompt without a new emulation sequence", () => {
    const current = snapshot("10", "2", false);
    expect(
      mergeRuntimeOverlaySnapshot(current, snapshot("10", "3", true))
        .resumeRequired,
    ).toBe(true);
  });
  test("a delayed snapshot cannot undo a focus transition", () => {
    const current = snapshot("10", "3", true);
    const merged = mergeRuntimeOverlaySnapshot(
      current,
      snapshot("11", "2", false),
    );
    expect(merged.sequence).toBe("11");
    expect(merged.resumeRequired).toBe(true);
    expect(merged.focusRevision).toBe("3");
  });
  test("resuming clears the prompt without regressing newer game data", () => {
    const merged = mergeRuntimeOverlaySnapshot(
      snapshot("12", "3", true),
      snapshot("11", "4", false),
    );
    expect(merged.sequence).toBe("12");
    expect(merged.resumeRequired).toBe(false);
  });
});
