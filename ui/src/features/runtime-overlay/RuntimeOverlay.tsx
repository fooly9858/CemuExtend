import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { subscribe } from "../../bridge/events";
import type {
  OverlayPosition,
  OverlayTextStyle,
  RuntimeOverlaySnapshot,
} from "../../bridge/contracts";
import { invoke } from "../../bridge/native";
import {
  mergeRuntimeOverlaySnapshot,
  overlayColor,
} from "./runtimeOverlayModel";

// Laid out like the console's own keyboard: four rows of ten, then a row of
// modifiers. Keeping every row the same width is what makes the grid line up.
const LETTER_ROWS = [
  ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
  ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
  ["a", "s", "d", "f", "g", "h", "j", "k", "l", "-"],
  ["z", "x", "c", "v", "b", "n", "m", "@", ".", "_"],
] as const;
const SYMBOL_ROWS = [
  ["!", '"', "#", "$", "%", "&", "'", "(", ")", "*"],
  ["+", ",", "-", ".", "/", ":", ";", "<", "=", ">"],
  ["?", "@", "[", "\\", "]", "^", "_", "`", "{", "}"],
  ["|", "~", "1", "2", "3", "4", "5", "6", "7", "8"],
] as const;

// Shift behaves as it does on the console: a tap capitalises the next character
// only, a second tap locks it until pressed again.
type ShiftMode = "off" | "once" | "lock";

const MIN_OVERLAY_SCALE = 50;
const MAX_OVERLAY_SCALE = 300;
const MAX_OVERLAY_VIEWPORT_SCALE = 4;
const PERCENT_SCALE = 100;

function styleProperties(style: OverlayTextStyle) {
  const scale = Math.min(
    MAX_OVERLAY_SCALE,
    Math.max(MIN_OVERLAY_SCALE, style.scale),
  );
  return {
    color: overlayColor(style.color),
    fontSize: `clamp(${MIN_OVERLAY_SCALE / PERCENT_SCALE}em, ${scale / PERCENT_SCALE}em, ${MAX_OVERLAY_VIEWPORT_SCALE}vmin)`,
  };
}

function Stats({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const { stats, visibility } = snapshot;
  const lines: Array<[string, string]> = [];
  if (visibility.fps) lines.push(["FPS", stats.fps.toFixed(2)]);
  if (visibility.drawCalls)
    lines.push([
      "Draws/f",
      `${stats.drawCalls.toLocaleString()} (fast: ${stats.fastDrawCalls.toLocaleString()})`,
    ]);
  if (visibility.cpuUsage) lines.push(["CPU", `${stats.cpuUsage.toFixed(2)}%`]);
  if (visibility.cpuPerCore)
    stats.cpuPerCore.forEach((usage, index) =>
      lines.push([`CPU #${index + 1}`, `${usage.toFixed(2)}%`]),
    );
  if (visibility.ramUsage)
    lines.push(["RAM", `${stats.ramUsageMb.toLocaleString()} MB`]);
  if (visibility.vramUsage && stats.vramUsageMb >= 0 && stats.vramTotalMb >= 0)
    lines.push([
      "VRAM",
      `${stats.vramUsageMb.toLocaleString()} / ${stats.vramTotalMb.toLocaleString()} MB`,
    ]);
  if (visibility.debug)
    stats.debugLines.forEach(({ label, value }) => lines.push([label, value]));
  if (!lines.length) return null;
  return (
    <dl className="runtime-overlay__panel runtime-overlay__stats">
      {lines.map(([label, value], index) => (
        <div key={`${label}-${index}`}>
          <dt>{label}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

function Notices({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const [receivedAt, setReceivedAt] = useState(() => Date.now());
  const [, setClock] = useState(0);
  useEffect(() => setReceivedAt(Date.now()), [snapshot.sequence]);
  useEffect(() => {
    if (!snapshot.notices.some((notice) => notice.remainingMs > 0)) return;
    const timer = window.setInterval(() => setClock((value) => value + 1), 100);
    return () => window.clearInterval(timer);
  }, [snapshot.notices]);
  const elapsed = Date.now() - receivedAt;
  const visible = snapshot.notices.filter(
    (notice) => notice.remainingMs === 0 || notice.remainingMs > elapsed,
  );
  if (!visible.length) return null;
  return (
    <div className="runtime-overlay__notices" aria-live="polite">
      {visible.map((notice) => (
        <div
          className={`runtime-overlay__panel runtime-overlay__notice runtime-overlay__notice--${notice.kind}`}
          key={notice.id}
        >
          <span className="runtime-overlay__notice-icon" aria-hidden="true">
            {notice.kind === "battery"
              ? "▱"
              : notice.kind === "controller"
                ? "⌁"
                : notice.kind === "account"
                  ? "●"
                  : notice.kind === "shader" || notice.kind === "pipeline"
                    ? "◌"
                    : "◆"}
          </span>
          <span>{notice.text}</span>
        </div>
      ))}
    </div>
  );
}

function ShaderProgress({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const progress = snapshot.shaderProgress;
  if (!progress.visible) return null;
  const percent = progress.total
    ? Math.min(100, Math.round((progress.current / progress.total) * 100))
    : 0;
  return (
    <section className="runtime-overlay__shader-progress" aria-live="polite">
      <div className="runtime-overlay__shader-card">
        <p>
          {progress.pipelines
            ? "Loading cached pipelines…"
            : "Loading cached shaders…"}
        </p>
        <progress max={Math.max(1, progress.total)} value={progress.current} />
        <strong>
          {progress.current.toLocaleString()} /{" "}
          {progress.total.toLocaleString()} ({percent}%)
        </strong>
      </div>
      {!progress.pipelines && (
        <dl className="runtime-overlay__shader-counts">
          <div>
            <dt>Vertex shaders</dt>
            <dd>{progress.vertexShaders}</dd>
          </div>
          <div>
            <dt>Pixel shaders</dt>
            <dd>{progress.pixelShaders}</dd>
          </div>
          <div>
            <dt>Geometry shaders</dt>
            <dd>{progress.geometryShaders}</dd>
          </div>
        </dl>
      )}
    </section>
  );
}

function SoftwareKeyboard({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const keyboard = snapshot.keyboard;
  const [shift, setShift] = useState<ShiftMode>(
    keyboard.shifted ? "lock" : "off",
  );
  const [symbols, setSymbols] = useState(false);
  useEffect(() => {
    setShift(keyboard.shifted ? "lock" : "off");
    setSymbols(false);
  }, [keyboard.generation, keyboard.shifted]);
  const submit = useCallback(
    (keyCode: number) =>
      invoke("overlay.submitKeyboardKey", {
        generation: keyboard.generation,
        keyCode,
      }),
    [keyboard.generation],
  );
  const press = useCallback(
    (character: string) => {
      const shifted = shift !== "off" ? character.toUpperCase() : character;
      void submit(shifted.codePointAt(0)!);
      // A one-shot capital applies to this character and nothing after it.
      setShift((mode) => (mode === "once" ? "off" : mode));
    },
    [shift, submit],
  );
  useEffect(() => {
    if (!keyboard.active) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.ctrlKey || event.metaKey || event.altKey) return;
      const keyCode =
        event.key === "Backspace"
          ? 8
          : event.key === "Enter"
            ? 13
            : Array.from(event.key).length === 1
              ? event.key.codePointAt(0)
              : undefined;
      if (keyCode === undefined) return;
      event.preventDefault();
      void submit(keyCode);
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [keyboard.active, submit]);
  if (!keyboard.active) return null;
  const rows = symbols ? SYMBOL_ROWS : LETTER_ROWS;
  return (
    <section
      className="runtime-overlay__modal-layer runtime-overlay__keyboard"
      role="dialog"
      aria-modal="true"
      aria-label="Software keyboard"
    >
      <div className="runtime-overlay__input-preview">
        <span>{keyboard.text}</span>
        <i aria-hidden="true" />
      </div>
      <div className="runtime-overlay__key-grid">
        {rows.map((row, rowIndex) => (
          <div className="runtime-overlay__key-row" key={rowIndex}>
            {row.map((key) => (
              <button
                key={key}
                // Type on the way down, the way a physical key behaves. The click
                // handler still fires for a synthesised click - detail 0, which is
                // how controller activation reaches a button - without doubling up
                // on a real press.
                onPointerDown={(event) => {
                  event.preventDefault();
                  press(key);
                }}
                onClick={(event) => {
                  if (event.detail === 0) press(key);
                }}
              >
                {shift !== "off" && !symbols ? key.toUpperCase() : key}
              </button>
            ))}
          </div>
        ))}
        <div className="runtime-overlay__key-row runtime-overlay__key-row--actions">
          <button
            className={`runtime-overlay__shift${shift === "lock" ? " is-locked" : ""}`}
            aria-pressed={shift !== "off"}
            aria-label={shift === "lock" ? "Caps lock on" : "Shift"}
            onClick={() =>
              setShift((mode) =>
                mode === "off" ? "once" : mode === "once" ? "lock" : "off",
              )
            }
          >
            ⇧
          </button>
          <button
            className="runtime-overlay__symbols"
            aria-pressed={symbols}
            onClick={() => setSymbols((value) => !value)}
          >
            {symbols ? "abc" : "@#:"}
          </button>
          <button
            className="runtime-overlay__space"
            aria-label="Space"
            onClick={() => void submit(32)}
          />
          <button
            className="runtime-overlay__backspace"
            aria-label="Backspace"
            onClick={() => void submit(8)}
          >
            ⌫
          </button>
          <button
            className="runtime-overlay__done"
            onClick={() => void submit(13)}
          >
            Done
          </button>
        </div>
      </div>
    </section>
  );
}

function ErrorDialog({ snapshot }: { snapshot: RuntimeOverlaySnapshot }) {
  const dialog = snapshot.errorDialog;
  if (!dialog.active) return null;
  const select = (rightButton: boolean) =>
    invoke("overlay.selectErrorButton", {
      generation: dialog.generation,
      rightButton,
    });
  return (
    <section
      className="runtime-overlay__modal-layer"
      role="dialog"
      aria-modal="true"
      aria-labelledby="runtime-error-title"
    >
      <div
        className="runtime-overlay__dialog"
        style={{ opacity: dialog.opacity }}
      >
        <h1 id="runtime-error-title">{dialog.title}</h1>
        <p>{dialog.message}</p>
        <div className="runtime-overlay__dialog-actions">
          <button autoFocus onClick={() => void select(false)}>
            {dialog.leftButton}
          </button>
          {dialog.rightButton && (
            <button onClick={() => void select(true)}>
              {dialog.rightButton}
            </button>
          )}
        </div>
      </div>
    </section>
  );
}

export function RuntimeOverlayRoot() {
  const [snapshot, setSnapshot] = useState<RuntimeOverlaySnapshot>();
  const rootRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const accept = (next: RuntimeOverlaySnapshot) =>
      setSnapshot((current) => mergeRuntimeOverlaySnapshot(current, next));
    const unsubscribe = subscribe((event) => {
      if (event.type === "overlay.changed")
        accept(event.payload as RuntimeOverlaySnapshot);
      else if (event.type === "emulation.loaded")
        setSnapshot((current) =>
          current
            ? {
                ...current,
                shaderProgress: {
                  ...current.shaderProgress,
                  visible: false,
                },
              }
            : current,
        );
    });
    void invoke("overlay.getSnapshot").then(accept);
    return unsubscribe;
  }, []);
  useEffect(() => {
    if (
      !snapshot ||
      snapshot.resumeRequired ||
      snapshot.interaction === "passive"
    )
      return;
    const navigate = (event: Event) => {
      const action = (event as CustomEvent<string>).detail;
      if (
        (action === "cancel" || action === "input") &&
        snapshot.interaction === "softwareKeyboard"
      ) {
        window.dispatchEvent(
          new KeyboardEvent("keydown", {
            key: action === "cancel" ? "Backspace" : "Enter",
          }),
        );
        return;
      }
      const buttons = Array.from(
        rootRef.current?.querySelectorAll<HTMLButtonElement>(
          "button:not(:disabled)",
        ) ?? [],
      );
      if (!buttons.length) return;
      const active = document.activeElement;
      const index = buttons.findIndex((button) => button === active);
      if (action === "activate") {
        if (index >= 0) buttons[index].click();
        else buttons[0].focus();
        return;
      }
      if (!["left", "right", "up", "down"].includes(action)) return;
      if (action === "up" || action === "down") {
        // The keys form a grid, so a vertical step has to land on the nearest key
        // in the row above or below. Moving one place in document order instead
        // made up and down behave exactly like left and right.
        const current = buttons[index < 0 ? 0 : index];
        const from = current.getBoundingClientRect();
        const fromCenter = from.left + from.width / 2;
        const rowOf = (button: HTMLButtonElement) => {
          const rect = button.getBoundingClientRect();
          return action === "up" ? -rect.bottom : rect.top;
        };
        // Keys are not all one column wide, so picking the nearest centre walks
        // past a wide key that sits directly below. Prefer whichever key actually
        // spans this column, and fall back to the nearest edge.
        const distanceFrom = (button: HTMLButtonElement) => {
          const rect = button.getBoundingClientRect();
          if (fromCenter >= rect.left && fromCenter <= rect.right) return 0;
          return fromCenter < rect.left
            ? rect.left - fromCenter
            : fromCenter - rect.right;
        };
        const target = buttons
          .filter((button) => {
            const rect = button.getBoundingClientRect();
            return action === "up"
              ? rect.bottom <= from.top + 1
              : rect.top >= from.bottom - 1;
          })
          .sort((left, right) => {
            const rows = rowOf(left) - rowOf(right);
            if (Math.abs(rows) > 1) return rows;
            return distanceFrom(left) - distanceFrom(right);
          })[0];
        (target ?? current).focus();
        return;
      }
      const delta = action === "left" ? -1 : 1;
      buttons[
        (index < 0 ? 0 : index + delta + buttons.length) % buttons.length
      ].focus();
    };
    window.addEventListener("cemu-overlay-navigate", navigate);
    return () => window.removeEventListener("cemu-overlay-navigate", navigate);
  }, [snapshot]);
  const groups = useMemo(() => {
    if (!snapshot) return new Map<OverlayPosition, React.ReactNode[]>();
    const result = new Map<OverlayPosition, React.ReactNode[]>();
    const add = (position: OverlayPosition, content: React.ReactNode) => {
      if (position === "disabled") return;
      const items = result.get(position) ?? [];
      items.push(content);
      result.set(position, items);
    };
    add(
      snapshot.overlayStyle.position,
      <div style={styleProperties(snapshot.overlayStyle)} key="stats">
        <Stats snapshot={snapshot} />
      </div>,
    );
    add(
      snapshot.notificationStyle.position,
      <div style={styleProperties(snapshot.notificationStyle)} key="notices">
        <Notices snapshot={snapshot} />
      </div>,
    );
    return result;
  }, [snapshot]);
  if (!snapshot) return <div className="runtime-overlay-root" />;
  return (
    <div
      ref={rootRef}
      className={`runtime-overlay-root runtime-overlay-root--${snapshot.interaction}`}
    >
      {snapshot.resumeRequired && (
        <div
          role="status"
          style={{
            position: "absolute",
            inset: 0,
            zIndex: 1000,
            display: "grid",
            placeItems: "center",
            background: "#0006",
            pointerEvents: "none",
          }}
        >
          <div
            style={{
              padding: "16px 24px",
              borderRadius: 12,
              color: "white",
              background: "#171a21",
              fontSize: 20,
            }}
          >
            クリックで操作を再開
          </div>
        </div>
      )}
      <ShaderProgress snapshot={snapshot} />
      {[...groups].map(([position, content]) => (
        <div
          className={`runtime-overlay__anchor runtime-overlay__anchor--${position}`}
          key={position}
        >
          {content}
        </div>
      ))}
      <SoftwareKeyboard snapshot={snapshot} />
      <ErrorDialog snapshot={snapshot} />
    </div>
  );
}
