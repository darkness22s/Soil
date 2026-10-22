export interface SoilInfo {
  id: string;
  name: string;
  version: string;
  kind: "webapp" | "webapp+native" | "installer";
  dataDir: string;
}

export interface SoilStore {
  get(key: string): Promise<{ value: string | null }>;
  set(key: string, value: string): Promise<Record<string, never>>;
  delete(key: string): Promise<Record<string, never>>;
  keys(): Promise<{ keys: string[] }>;
}

export interface SoilApi {
  app: { info(): Promise<SoilInfo> };
  window: {
    setTitle(title: string): Promise<Record<string, never>>;
    close(): Promise<Record<string, never>>;
    setSize(width: number, height: number): Promise<Record<string, never>>;
    minimize(): Promise<Record<string, never>>;
  };
  dialog: {
    openFile(options?: {
      title?: string;
      filters?: Array<{ name: string; patterns: string[] }>;
    }): Promise<{ path: string } | null>;
    saveFile(options?: {
      title?: string;
      defaultName?: string;
    }): Promise<{ path: string } | null>;
  };
  clipboard: {
    read(): Promise<{ text: string }>;
    write(text: string): Promise<Record<string, never>>;
  };
  shell: { openExternal(url: string): Promise<Record<string, never>> };
  notify(title: string, body: string): Promise<Record<string, never>>;
  store: SoilStore;
  exec: { run(path: string, args: string[]): Promise<{ pid: number }> };
}

declare global {
  interface Window {
    soil: SoilApi;
  }
}

export {};
