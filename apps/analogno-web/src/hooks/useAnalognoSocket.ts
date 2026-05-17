import { useEffect, useRef, useState } from "react";
import type { ConnectionState, LibraryState, RuntimeState } from "../types/runtime";

function defaultWebsocketUrl(): string {
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  const host = window.location.hostname;
  const runtimeHost = host === "" || host === "localhost" ? "127.0.0.1" : host;
  return `${protocol}://${runtimeHost}:8765`;
}

const websocketUrl =
  import.meta.env.VITE_ANALOGNO_WS_URL ??
  defaultWebsocketUrl();

type ServerMessage = RuntimeState | LibraryState;

export function useAnalognoSocket() {
  const [connection, setConnection] = useState<ConnectionState>("connecting");
  const [socket, setSocket] = useState<WebSocket | null>(null);
  const [runtime, setRuntime] = useState<RuntimeState | null>(null);
  const [library, setLibrary] = useState<LibraryState | null>(null);
  const runtimeRef = useRef<RuntimeState | null>(null);

  useEffect(() => {
    let ws: WebSocket | null = null;
    let retryDelay = 250;
    let stopped = false;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;

    function scheduleReconnect() {
      if (stopped || retryTimer !== null) return;
      retryTimer = setTimeout(() => {
        retryTimer = null;
        retryDelay = Math.min(retryDelay * 1.5, 3000);
        connect();
      }, retryDelay);
    }

    function connect() {
      if (stopped) return;
      if (retryTimer !== null) {
        clearTimeout(retryTimer);
        retryTimer = null;
      }
      setConnection("connecting");
      const nextWs = new WebSocket(websocketUrl);
      ws = nextWs;

      nextWs.addEventListener("open", () => {
        if (stopped || ws !== nextWs) return;
        retryDelay = 250;
        setConnection("online");
        setSocket(nextWs);
      });

      nextWs.addEventListener("close", () => {
        if (stopped || ws !== nextWs) return;
        setConnection("offline");
        setSocket(null);
        scheduleReconnect();
      });

      nextWs.addEventListener("error", () => {
        if (stopped || ws !== nextWs) return;
        setConnection("offline");
        setSocket(null);
      });

      nextWs.addEventListener("message", (event: MessageEvent<string>) => {
        if (stopped || ws !== nextWs) return;
        const parsed = JSON.parse(event.data) as ServerMessage;
        if (parsed.type === "runtime") {
          runtimeRef.current = parsed;
          setRuntime(parsed);
        } else if (parsed.type === "library") {
          setLibrary(parsed);
        }
      });
    }

    function reconnectNow() {
      if (stopped) return;
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
      retryDelay = 250;
      connect();
    }

    connect();
    window.addEventListener("focus", reconnectNow);
    window.addEventListener("online", reconnectNow);
    document.addEventListener("visibilitychange", reconnectNow);

    return () => {
      stopped = true;
      if (retryTimer !== null) clearTimeout(retryTimer);
      window.removeEventListener("focus", reconnectNow);
      window.removeEventListener("online", reconnectNow);
      document.removeEventListener("visibilitychange", reconnectNow);
      ws?.close();
      ws = null;
    };
  }, []);

  return { connection, socket, runtime, runtimeRef, library };
}
