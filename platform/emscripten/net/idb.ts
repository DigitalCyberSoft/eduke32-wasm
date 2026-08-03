// ─────────────────────────────────────────────────────────────────────────────
// IDB — IndexedDB persistence for downloaded GRPs.
//
// A GRP a joiner downloads from a host is written here ONLY AFTER it hashes to the
// advertised fingerprint (see grp.ts GrpReceiver.verify). On the next boot, the
// page reads it back, writes it into Module.FS, and relaunches the engine with
// -gamegrp so the joiner is now on the host's GRP and can rejoin.
//
// Keyed by CRC-32 (unsigned) so the same GRP is stored once regardless of file name.
// ─────────────────────────────────────────────────────────────────────────────

const DB_NAME = "eduke32-net";
const STORE = "grps";
const DB_VERSION = 1;

export interface StoredGrp {
  crc: number; // unsigned CRC-32 (primary key)
  sha256: string;
  size: number;
  filename: string; // suggested engine file name (e.g. "DUKE.GRP")
  name: string; // human label
  bytes: Uint8Array; // the verified GRP bytes
  savedAt: number;
}

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") {
      reject(new Error("IndexedDB unavailable"));
      return;
    }
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, { keyPath: "crc" });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error("indexedDB.open failed"));
  });
}

function tx<T>(mode: IDBTransactionMode, fn: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  return openDb().then(
    (db) =>
      new Promise<T>((resolve, reject) => {
        const t = db.transaction(STORE, mode);
        const req = fn(t.objectStore(STORE));
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error ?? new Error("idb request failed"));
        t.oncomplete = () => db.close();
      }),
  );
}

/** Persist a verified GRP. crc must be unsigned. */
export async function putGrp(g: StoredGrp): Promise<void> {
  await tx("readwrite", (s) => s.put({ ...g, crc: g.crc >>> 0 }));
}

/** Read a stored GRP by unsigned CRC, or null. */
export async function getGrp(crc: number): Promise<StoredGrp | null> {
  try {
    const v = await tx<StoredGrp | undefined>("readonly", (s) => s.get(crc >>> 0));
    return v ?? null;
  } catch {
    return null;
  }
}

/** List stored GRPs (metadata + bytes). */
export async function listGrps(): Promise<StoredGrp[]> {
  try {
    return await tx<StoredGrp[]>("readonly", (s) => s.getAll() as IDBRequest<StoredGrp[]>);
  } catch {
    return [];
  }
}

export async function deleteGrp(crc: number): Promise<void> {
  try {
    await tx("readwrite", (s) => s.delete(crc >>> 0));
  } catch {
    /* ignore */
  }
}
