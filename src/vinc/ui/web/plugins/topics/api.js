// Per-page bearer capability. It is never placed in URLs or persistent browser storage.
let token;
const requestTimeoutMs = 3000;
const sessionAttempts = 2;

export async function read(path, signal) {
    const timeout = AbortSignal.timeout(requestTimeoutMs);
    const combined = AbortSignal.any([signal, timeout]);
    for (let attempt = 0; attempt < sessionAttempts; ++attempt) {
        if (!token) {
            const response = await fetch("/api/session", {
                headers: { "X-Vinc-UI": "1" }, signal: combined,
                credentials: "omit", cache: "no-store",
            });
            if (!response.ok) {
                throw new Error("Could not establish UI session");
            }
            const session = await response.json();
            // Accept the native service's bounded 128..512-bit capability format.
            if (typeof session.token !== "string" || !/^[a-f0-9]{32,128}$/.test(session.token) || session.token.length % 2 !== 0) {
                throw new Error("Invalid UI session response");
            }
            token = session.token;
        }
        const response = await fetch(path, {
            headers: { Authorization: `Bearer ${token}` }, signal: combined,
            credentials: "omit", cache: "no-store",
        });
        if (response.status === 401) {
            token = undefined;
            continue;
        }
        if (!response.ok) {
            throw new Error(`Topic service unavailable (HTTP ${response.status})`);
        }
        return response.json();
    }
    throw new Error("UI session expired; refresh this page");
}
