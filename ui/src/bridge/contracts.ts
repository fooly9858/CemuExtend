import { implementedWindowRoles, windowRoles } from "../generated/roles";
export { implementedWindowRoles, windowRoles };

export type WindowRole = (typeof windowRoles)[number];
export type ImplementedToolWindowRole = (typeof implementedWindowRoles)[number];
export type ActiveWindowRole =
  "main-library" | "runtime-overlay" | ImplementedToolWindowRole;

export type RpcRequest = { id: string; method: string; params: unknown };
export type RpcError = { code: string; message: string; details?: unknown };
export type RpcResponse<T = unknown> =
  | { id: string; ok: true; result: T }
  | { id: string; ok: false; error: RpcError };

export type Bootstrap = {
  windowId: string;
  windowRole: ActiveWindowRole;
  appVersion: string;
  platform: string;
  activeAccountName: string;
  theme: "light" | "dark" | "system";
  themeRevision: string;
  language: string;
  languageRevision: string;
  shuttingDown: boolean;
  context?: { titleId?: string; packageKey?: string; generation?: string };
};

export type CemodPermission = {
  name: string;
  bit: string;
  requested: boolean;
  granted: boolean;
  dangerous: boolean;
  manifestMismatch: boolean;
};
export type CemodPackage = {
  packageKey: string;
  titleIds: string[];
  modId: string;
  principal: string;
  modIdentity: string;
  packageDigest: string;
  pluginName: string;
  author: string;
  version: string;
  description: string;
  scope: string;
  status: string;
  approvalReason: string;
  warnings: string[];
  permissions: CemodPermission[];
  requestedPermissions: string;
  grantedPermissions: string;
  approved: boolean;
  signedPackage: boolean;
  trustedNative: boolean;
  wups: boolean;
  headless: boolean;
  runtimeAvailable: boolean;
  valid: boolean;
  enabled: boolean;
  trustUpdates: boolean;
};
export type CemodManagerSnapshot = {
  generation: string;
  selectedTitleId: string | null;
  packages: CemodPackage[];
  cancelled: boolean;
};
export type CemodManagerResult = {
  ok: boolean;
  error:
    | "none"
    | "conflict"
    | "notFound"
    | "invalidPermissions"
    | "inspectionFailed"
    | "saveFailed"
    | "importFailed";
  diagnostic: string;
  snapshot: CemodManagerSnapshot;
};
export type CemodApprovalUpdate = {
  generation: string;
  titleId: string;
  packageKey: string;
  grantedPermissions: string;
  approved: boolean;
  trustUpdates: boolean;
};

export type ChecksumContent = {
  locationUid: string;
  titleId: string;
  name: string;
  version: number;
  region: string;
  type: "base" | "update" | "dlc" | "system";
  format: "folder" | "wud" | "nus" | "wua" | "wuhb";
};
export type ChecksumModel = { entries: ChecksumContent[] };
export type TitleManagerEntry = Omit<ChecksumContent, "type"> & {
  type: ChecksumContent["type"] | "save";
  path: string;
  canLaunch: boolean;
  canVerify: boolean;
  canConvert: boolean;
  canDelete: boolean;
};

export type MemoryValueType =
  "int8" | "int16" | "int32" | "int64" | "float32" | "float64";
export type MemoryTypedValue = { type: MemoryValueType; text: string };
export type MemoryAddress = { space: "wiiu-virtual"; value: string };
export type MemorySearchSession = {
  sessionToken: string;
  generation: string;
  mapGeneration: string;
  bytesTotal: number;
};
export type MemorySearchStatus = {
  generation: string;
  state: "scanning" | "complete" | "cancelled" | "failed";
  bytesScanned: number;
  bytesTotal: number;
  resultCount: number;
  resultCapReached: boolean;
  scanCapReached: boolean;
  diagnostic: string;
};
export type MemorySearchPage = {
  generation: string;
  offset: number;
  total: number;
  results: Array<{ address: MemoryAddress; value: MemoryTypedValue }>;
};
export type GuestAddress = string;
export type PpcDebuggerControl = "break" | "run" | "stepInto" | "stepOver";
export type PpcDebuggerInstruction = {
  address: GuestAddress;
  opcode: string;
  mnemonic: string;
  operands: string;
  current: boolean;
  breakpoint: boolean;
};
export type PpcDebuggerBreakpoint = {
  identity: string;
  address: GuestAddress;
  enabled: boolean;
  logging: boolean;
};
export type PpcDebuggerSnapshot = {
  generation: string;
  available: boolean;
  trapped: boolean;
  instructionPointer: GuestAddress;
  linkRegister: GuestAddress;
  gpr: GuestAddress[];
  instructions: PpcDebuggerInstruction[];
  breakpoints: PpcDebuggerBreakpoint[];
  breakpointCapReached: boolean;
  diagnostic: string;
};
export type TitleManagerModel = {
  scanning: boolean;
  entries: TitleManagerEntry[];
};
export type NativeSelection =
  | { cancelled: true }
  | { cancelled: false; sourceToken: string; displayName: string };
export type NativeDestination =
  { cancelled: true } | { cancelled: false; destinationToken: string };
export type TitleInstallPlanView = {
  planToken: string;
  titleId: string;
  titleName: string;
  version: number;
  kind:
    | "unknown"
    | "base"
    | "demo"
    | "update"
    | "dlc"
    | "systemTitle"
    | "systemData";
  conflict: "none" | "differentType" | "sameVersion" | "newerVersionInstalled";
  requiredBytes: number;
  availableBytes: number;
};
export type WuaConversionPlanView = {
  planToken: string;
  suggestedFileName: string;
  items: Array<{
    titleId: string;
    version: number;
    role: "base" | "update" | "dlc";
    displayPath: string;
  }>;
};
export type ManagedContentDeletePlanView = {
  planToken: string;
  titleId: string;
  name: string;
  displayPath: string;
};

export type SaveEntryState = "missing" | "directory" | "nonDirectory";
export type SaveIdentity = { titleId: string; persistentId: string };
export type SaveAccount = { persistentId: string; name: string };
export type SaveLocation = SaveIdentity & {
  state: SaveEntryState;
  accountName: string;
};
export type SaveTitle = {
  titleId: string;
  name: string;
  saves: Array<Omit<SaveLocation, "titleId">>;
};
export type SaveManagerModel = {
  scanning: boolean;
  accounts: SaveAccount[];
  titles: SaveTitle[];
};
export type SaveImportInspection = {
  confirmationToken: string;
  targetState: SaveEntryState;
  sourceTitleId: string | null;
  titleMismatch: boolean;
};

export type PpcThreadState =
  | "none"
  | "ready"
  | "running"
  | "waiting"
  | "moribund"
  | "suspended"
  | "unknown";
export type PpcThread = {
  address: string;
  identity: string;
  entryPoint: string;
  stackLow: string;
  stackHigh: string;
  instructionPointer: string;
  linkRegister: string;
  state: PpcThreadState;
  requestedAffinity: number;
  effectiveAffinity: number;
  basePriority: number;
  effectivePriority: number;
  wakeUpTime: string;
  totalCycles: string;
  name: string;
  gpr: [string, string, string, string, string];
  cancelRequested: boolean;
  suspensionOwnedByFacade: boolean;
  waitingMutex?: { address: string; owner: string; lockCount: number };
};
export type PpcThreadsModel = {
  generation: string;
  available: boolean;
  diagnostic: string;
  threads: PpcThread[];
};
export type PpcThreadCommand =
  "suspend" | "resume" | "boost1" | "boost5" | "decrease1" | "decrease5";
export type PpcThreadCommandResult = { applied: boolean; diagnostic: string };

export type Title = {
  titleId: string;
  name: string;
  path: string;
  region: string;
  version: number;
  playTimeMinutes: number;
  lastPlayed: string | null;
  iconDataUrl?: string;
};

export type TitleLaunchResult = {
  status: "started" | "awaitingPermission";
  titleId: string;
};

export type TitleLaunchState = {
  status:
    | "awaitingPermission"
    | "started"
    | "permissionDenied"
    | "cancelled"
    | "failed"
    | "shutdown";
  titleId: string;
  packageKey?: string;
  diagnostic?: string;
};

export type TitleLaunchStateEvent = NativeEvent & {
  type: "titles.launchState";
  payload: TitleLaunchState;
};

export type NativeEvent = { type: string; sequence: string; payload: unknown };

export type OverlayPosition =
  | "disabled"
  | "topLeft"
  | "topCenter"
  | "topRight"
  | "bottomLeft"
  | "bottomCenter"
  | "bottomRight";
export type OverlayTextStyle = {
  position: OverlayPosition;
  color: number;
  scale: number;
};
export type RuntimeOverlaySnapshot = {
  resumeRequired?: boolean;
  focusRevision?: string;
  sequence: string;
  overlayStyle: OverlayTextStyle;
  notificationStyle: OverlayTextStyle;
  visibility: {
    fps: boolean;
    drawCalls: boolean;
    cpuUsage: boolean;
    cpuPerCore: boolean;
    ramUsage: boolean;
    vramUsage: boolean;
    debug: boolean;
  };
  stats: {
    fps: number;
    drawCalls: number;
    fastDrawCalls: number;
    cpuUsage: number;
    cpuPerCore: number[];
    ramUsageMb: number;
    vramUsageMb: number;
    vramTotalMb: number;
    debugLines: Array<{ label: string; value: string }>;
  };
  notices: Array<{
    id: string;
    kind:
      | "account"
      | "controller"
      | "friend"
      | "battery"
      | "shader"
      | "pipeline"
      | "message";
    text: string;
    player?: number;
    remainingMs: number;
  }>;
  shaderProgress: {
    generation: string;
    visible: boolean;
    pipelines: boolean;
    current: number;
    total: number;
    vertexShaders: number;
    pixelShaders: number;
    geometryShaders: number;
    backgroundImageAvailable: boolean;
  };
  keyboard: {
    generation: string;
    active: boolean;
    keyboardOnly: boolean;
    shifted: boolean;
    maximumLength: number;
    text: string;
  };
  errorDialog: {
    generation: string;
    active: boolean;
    title: string;
    message: string;
    leftButton: string;
    rightButton: string;
    opacity: number;
  };
  interaction: "passive" | "softwareKeyboard" | "errorDialog";
};

export type RuntimeOverlayChangedEvent = NativeEvent & {
  type: "overlay.changed";
  payload: RuntimeOverlaySnapshot;
};

export type LoggingLevel = "info" | "warning" | "error";
export type LoggingEntry = {
  sequence: string;
  level: LoggingLevel;
  category: string;
  message: string;
};
export type LoggingSnapshot = {
  entries: LoggingEntry[];
  firstAvailableSequence: string;
  nextSequence: string;
  droppedEntries: string;
  retainedBytes: string;
  truncated: boolean;
};
export type LoggingEntriesEvent = NativeEvent & {
  type: "logging.entries";
  payload: LoggingSnapshot;
};
export type LoggingClearedEvent = NativeEvent & {
  type: "logging.cleared";
  payload: { clearedThroughSequence: string };
};

export type UsbDeviceDescriptor = {
  id: string;
  vendorId: number;
  productId: number;
  interfaceIndex: number;
  interfaceSubClass: number;
  protocol: number;
  maxPacketSizeRx: number;
  maxPacketSizeTx: number;
  opened: boolean;
};
export type EmulatedUsbDeviceId = "skylanders" | "infinity" | "dimensions";
export type EmulatedUsbDevice = {
  id: EmulatedUsbDeviceId;
  name: string;
  vendorId: number;
  productId: number;
  enabled: boolean;
  connected: boolean;
};
export type EmulatedUsbModel = {
  generation: string;
  emulatedDevices: EmulatedUsbDevice[];
  attachedDevices: UsbDeviceDescriptor[];
};
export type UsbDevicesChangedPayload = {
  generation: string;
  attached: boolean;
  device: UsbDeviceDescriptor;
};

export type AboutInfo = {
  name: string;
  version: string;
  commit: string;
  buildDate: string;
  frontend: "cef-react";
  browserEngine: string;
  originalAuthors: string[];
  libraries: Array<{ name: string; license: string; url: string }>;
  links: Array<{ label: string; url: string }>;
};

export type Account = {
  persistentId: number;
  persistentIdHex: string;
  miiName: string;
  birthYear: number;
  birthMonth: number;
  birthDay: number;
  gender: number;
  email: string;
  country: number;
  validOnlineAccount: boolean;
};

export type AccountCountry = { code: number; name: string };

export type AccountManagerModel = {
  accounts: Account[];
  countries: AccountCountry[];
  nextPersistentId: number;
  hasFreeSlots: boolean;
  activePersistentId: number;
  titleRunning: boolean;
  networkSettings: Array<{
    persistentId: number;
    service: "offline" | "nintendo" | "pretendo" | "custom" | "plasma";
    validation: {
      validAccount: boolean;
      otp: "missing" | "corrupted" | "ok";
      seeprom: "missing" | "corrupted" | "ok";
      missingFiles: string[];
      accountError:
        | "none"
        | "noAccountId"
        | "noPasswordCached"
        | "passwordCacheEmpty"
        | "noPrincipalId";
    };
  }>;
  onlineEnvironment: {
    requiredFilesAvailable: boolean;
    otpPresent: boolean;
    seepromPresent: boolean;
    consoleCertificateAvailable: boolean;
  };
};

export type AccountUpdate = Omit<
  Account,
  "persistentIdHex" | "validOnlineAccount"
>;
export type AccountNetworkService =
  AccountManagerModel["networkSettings"][number]["service"];

export type GraphicPackPreset = {
  category: string;
  name: string;
  active: boolean;
  visible: boolean;
};

export type GraphicPack = {
  key: string;
  virtualPath: string;
  name: string;
  description: string;
  version: number;
  universal: boolean;
  enabled: boolean;
  activated: boolean;
  defaultEnabled: boolean;
  hasShaders: boolean;
  hasPatches: boolean;
  hasCustomVsync: boolean;
  supportedVersion: boolean;
  titleIds: string[];
  presetOrder: string[];
  presets: GraphicPackPreset[];
};

export type GraphicPackMutation = {
  changed: boolean;
  titleRunning: boolean;
  requiresRestart: boolean;
  applied: boolean;
  reloaded: boolean;
  diagnostic: string;
  info?: GraphicPack;
};

export type GraphicPackInstallKind = "community" | "customUrl";
export type GraphicPackInstallRequest = {
  kind: GraphicPackInstallKind;
  url?: string;
  replaceExisting: boolean;
};

export type TitleInstallSelection = {
  planToken: string;
  titleId: string;
  titleName: string;
  version: number;
  kind:
    | "unknown"
    | "base"
    | "demo"
    | "update"
    | "dlc"
    | "systemTitle"
    | "systemData";
  conflict: "none" | "differentType" | "sameVersion" | "newerVersionInstalled";
  installedVersion: number | null;
  requiredBytes: number;
  availableBytes: number;
};

export type UpdateManagerModel = { titleRunning: boolean };

export type FrontendSettings = {
  revision: number;
  gamePaths: string[];
  startFullscreen: boolean;
  openPad: boolean;
  checkUpdates: boolean;
  saveScreenshots: boolean;
  updateChecksSupported: boolean;
  portableMode: boolean;
  titleRunning: boolean;
  setupCompleted: boolean;
  fullscreenOverride: boolean | null;
  crashDump: string;
  crashDumpChoices: string[];
};

export type FrontendSettingsUpdate = Pick<
  FrontendSettings,
  | "revision"
  | "gamePaths"
  | "startFullscreen"
  | "openPad"
  | "checkUpdates"
  | "saveScreenshots"
  | "crashDump"
> & {
  completeSetup: boolean;
};

export type FrontendSettingsApplyResult = {
  ok: boolean;
  error:
    | "none"
    | "conflict"
    | "titleRunning"
    | "fullscreenOverride"
    | "updateUnsupported"
    | "invalidPath"
    | "storageFailed"
    | "saveFailed";
  snapshot: FrontendSettings;
  diagnostic: string;
};

export type EmulatedControllerType =
  "disabled" | "gamePad" | "proController" | "classicController" | "wiimote";
export type ControllerAxisSettings = { deadzone: number; range: number };
export type PhysicalControllerSettings = {
  axis: ControllerAxisSettings;
  rotation: ControllerAxisSettings;
  trigger: ControllerAxisSettings;
  rumble: number;
  motion: boolean;
  packetDelay?: number;
};
export type PhysicalController = {
  token: number;
  api: string;
  displayName: string;
  connected: boolean;
  hasBattery: boolean;
  lowBattery: boolean;
  hasMotion: boolean;
  hasRumble: boolean;
  wiimoteExtension?: "none" | "nunchuck" | "classic" | "motionPlus";
  settings: PhysicalControllerSettings;
};
export type CapturedInputButton = { id: number; label: string };
export type InputMapping = {
  mappingId: number;
  label: string;
  binding: string;
  controllerToken?: number;
};
export type InputPlayer = {
  player: number;
  type: EmulatedControllerType;
  gameProfileLocked: boolean;
  profileName: string;
  controllers: PhysicalController[];
  mappings: InputMapping[];
};
export type InputSettingsModel = {
  generation: number;
  players: InputPlayer[];
  profiles: string[];
  availableApis: string[];
};
export type InputDeviceCandidate = {
  token: number;
  api: string;
  displayName: string;
  connected: boolean;
};

export type HotkeyAction =
  | "toggleFullscreen"
  | "toggleFullscreenAlternative"
  | "exitFullscreen"
  | "takeScreenshot"
  | "toggleFastForward"
  | "endEmulation"
  | "exitApplication";
export type HotkeyBinding = {
  action: HotkeyAction;
  keyboardUsage: number;
  keyboardModifiers: number;
  controllerButton: number | null;
  controllerLabel: string;
};
export type HotkeySettingsModel = {
  revision: number;
  controllerModifier: number | null;
  controllerModifierLabel: string;
  controller: { token: number; displayName: string } | null;
  bindings: HotkeyBinding[];
};
export type HotkeySettingsUpdate = {
  revision: number;
  controllerModifier: number | null;
  bindings: Array<Omit<HotkeyBinding, "controllerLabel">>;
};
export type HotkeySettingsApplyResult = {
  ok: boolean;
  error:
    "none" | "conflict" | "invalidBinding" | "duplicateBinding" | "saveFailed";
  snapshot: HotkeySettingsModel;
  diagnostic: string;
};

export type DiagnosticPageRequest = {
  generation: string;
  offset: number;
  limit: number;
};
export type TextureDiagnosticRow = {
  id: string;
  parentId?: string;
  kind: "texture" | "view";
  active: boolean;
  updatedOnGpu: boolean;
  depthFormat: boolean;
  dimension: string;
  format: string;
  width: number;
  height: number;
  depth: number;
  pitch: number;
  tileMode: number;
  firstSlice: number;
  sliceCount: number;
  firstMip: number;
  mipCount: number;
  ageMilliseconds: number;
  alternativeViewCount: number;
  resolutionOverridden: boolean;
  effectiveWidth: number;
  effectiveHeight: number;
  effectiveDepth: number;
};
export type TextureDiagnosticPage = {
  generation: string;
  offset: number;
  total: number;
  truncated: boolean;
  available: boolean;
  diagnostic: string;
  rows: TextureDiagnosticRow[];
};
export type AudioVoiceDiagnosticRow = {
  id: string;
  index: number;
  format: string;
  currentOffset: number;
  loopOffset: number;
  endOffset: number;
  looping: boolean;
  volume: number;
  volumeDelta: number;
  sourceRatio: number;
  lowPassEnabled: boolean;
  biquadEnabled: boolean;
  deviceMix: string;
};
export type AudioVoiceDiagnosticPage = {
  generation: string;
  offset: number;
  total: number;
  available: boolean;
  diagnostic: string;
  rows: AudioVoiceDiagnosticRow[];
};

declare global {
  interface Window {
    cemuInvoke?: (requestJson: string) => Promise<string>;
    __CEMU_BOOTSTRAP__?: Partial<Bootstrap>;
    __cemuDispatchEvent?: (event: NativeEvent) => void;
  }
}
