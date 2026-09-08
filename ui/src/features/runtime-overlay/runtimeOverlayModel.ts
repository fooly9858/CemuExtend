import type { RuntimeOverlaySnapshot } from "../../bridge/contracts";

export function overlayColor(color: number): string {
  void color;
  return "var(--color-ink)";
}

// Focus changes do not increment the emulation overlay's sequence. Merge the
// two streams independently so a late initial RPC cannot hide a resume prompt.
export function mergeRuntimeOverlaySnapshot(
  current: RuntimeOverlaySnapshot | undefined,
  next: RuntimeOverlaySnapshot,
): RuntimeOverlaySnapshot {
  if (!current) return next;
  const focus =
    BigInt(next.focusRevision ?? "0") >= BigInt(current.focusRevision ?? "0")
      ? next
      : current;
  return {
    ...(BigInt(next.sequence) >= BigInt(current.sequence) ? next : current),
    resumeRequired: focus.resumeRequired,
    focusRevision: focus.focusRevision,
  };
}
