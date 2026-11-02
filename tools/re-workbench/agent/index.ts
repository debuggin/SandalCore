declare const Stalker: { unfollow(): void; flush(): void } | undefined;
declare const rpc: { exports: Record<string, unknown> } | undefined;

export interface ProbeTraceResult {
    readonly scriptStructName: string;
    readonly bitPosBefore: number;
    readonly bitPosAfter: number;
    readonly excludedModules: readonly string[];
    readonly ok: boolean;
}
export interface SyntheticBunchResult {
    readonly channelIndex: number;
    readonly bitPosAfter: number;
    readonly totalBits: number;
    readonly isError: boolean;
}
const EXCLUDED_SYSTEM_MODULES: readonly string[] = ["ntdll.dll", "kernel32.dll", "KernelBase.dll", "ucrtbase.dll", "d3d12.dll"];

export function unfollowAndFlush(): boolean {
    if (typeof Stalker !== "undefined") {
        Stalker.unfollow();
        Stalker.flush();
    }
    return true;
}
export function probeStructNetSerialize(scriptStructName: string, sampleHexBytes: string): ProbeTraceResult {
    const bitLen = (sampleHexBytes.length >> 1) * 8;
    unfollowAndFlush();
    return { scriptStructName, bitPosBefore: 0, bitPosAfter: bitLen, excludedModules: EXCLUDED_SYSTEM_MODULES, ok: scriptStructName.length > 0 };
}
export const rpcExports = {
    probeStructNetSerialize,
    unfollowAndFlush,
    dumpClassNetCache(classNameFilter: string): ReadonlyArray<Record<string, unknown>> { return [{ classNameFilter, status: "ready" }]; },
    readDataTableRows(dataTablePath: string): ReadonlyArray<Record<string, unknown>> { return [{ dataTablePath, rows: 0 }]; },
    getActorProperties(actorNetGUID: number): Record<string, unknown> { return { actorNetGUID, properties: {} }; },
    injectSyntheticBunch(channelIndex: number, rawBitsHex: string): SyntheticBunchResult {
        const totalBits = (rawBitsHex.length >> 1) * 8;
        return { channelIndex, bitPosAfter: totalBits, totalBits, isError: false };
    },
    invokeClientRpc(actorNetGUID: number, functionName: string, argsJson: string): boolean {
        return actorNetGUID > 0 && functionName.length > 0 && argsJson.length >= 2;
    },
} as const;
if (typeof rpc !== "undefined") {
    rpc.exports = rpcExports;
}
