// Commits every function's parameters and return to the database, callee before caller.
//
// Without a committed prototype the decompiler guesses a callee's arguments at each call site
// and drops those whose setup it cannot tie to the call: 1,079 of the translator's stubs were
// "the decompiler lost a call's arguments" (2026-10-15). This runs Ghidra's own Decompiler
// Parameter ID (DecompilerParameterIdCmd) over every function: an acyclic call graph, decompiled
// leaves first on the analysis thread pool, each result's parameters committed before its callers
// are decompiled. Only storage is committed (the "paramid" style), not guessed data types, so the
// corpus's types stay what they were; signatures above ANALYSIS (user-defined, imported) are kept.
//
// Usage (headless): tools/ghidra/ghidra.sh script CommitPrototypes.java [timeoutSecs]
// Re-export the corpus afterwards (python tools/corpus.py --export).
//@category FrostLite
import ghidra.app.cmd.function.DecompilerParameterIdCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.util.task.WrappingTaskMonitor;

public class CommitPrototypes extends GhidraScript {

    private int committed() {
        int n = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (!f.isThunk() && f.getSignatureSource() != SourceType.DEFAULT) {
                n++;
            }
        }
        return n;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int timeout = args.length > 0 ? Integer.parseInt(args[0]) : 60;

        AddressSet entries = new AddressSet();
        int total = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (f.isThunk() || f.isExternal()) {
                continue;
            }
            entries.add(f.getEntryPoint());
            total++;
        }
        println("CommitPrototypes: " + total + " functions, " + committed() + " with a committed signature before");

        final long start = System.currentTimeMillis();
        WrappingTaskMonitor progress = new WrappingTaskMonitor(monitor) {
            private long done = 0;

            @Override
            public synchronized void incrementProgress(long amount) {
                super.incrementProgress(amount);
                done += amount;
                if (done % 2000 == 0) {
                    long s = (System.currentTimeMillis() - start) / 1000;
                    println("CommitPrototypes: " + done + " done, " + s + "s");
                }
            }
        };

        DecompilerParameterIdCmd cmd = new DecompilerParameterIdCmd("CommitPrototypes", entries,
                SourceType.ANALYSIS, false, false, timeout);
        boolean ok = cmd.applyTo(currentProgram, progress);
        long s = (System.currentTimeMillis() - start) / 1000;
        println("CommitPrototypes: " + (ok ? "finished" : "failed: " + cmd.getStatusMsg()) + " in " + s + "s, "
                + committed() + " with a committed signature after");
    }
}
