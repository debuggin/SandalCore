export interface BitCursorSample {
    readonly packetId: number;
    readonly bunchId: number;
    readonly totalBits: number;
    readonly consumedBits: number;
    readonly leftoverBits: number;
    readonly isError: boolean;
}

export interface ReBacklogEntry {
    readonly priorityId: string;
    readonly structName: string;
    readonly observedRuntimeCalls: number;
    readonly status: "MISSING_CODEC" | "IMPLEMENTED";
}

export interface ReInspectorModel {
    readonly route: "/re";
    readonly totalBunches: number;
    readonly zeroResidueBunches: number;
    readonly divergentBunches: number;
    readonly missingCodecs: number;
    readonly samples: readonly BitCursorSample[];
    readonly backlog: readonly ReBacklogEntry[];
}

export function createReInspectorModel(
    samples: readonly BitCursorSample[],
    backlog: readonly ReBacklogEntry[],
): ReInspectorModel {
    const zeroResidueBunches = samples.filter((s) => !s.isError && s.leftoverBits === 0).length;
    const missingCodecs = backlog.filter((b) => b.status === "MISSING_CODEC").length;
    return {
        route: "/re",
        totalBunches: samples.length,
        zeroResidueBunches,
        divergentBunches: samples.length - zeroResidueBunches,
        missingCodecs,
        samples,
        backlog,
    };
}

export function renderReInspectorSummary(model: ReInspectorModel): string {
    return `[${model.route}] bunches=${model.zeroResidueBunches}/${model.totalBunches} zero-residue | divergent=${model.divergentBunches} | missing_codecs=${model.missingCodecs}`;
}
