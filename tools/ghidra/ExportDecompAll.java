// FrostLite copy of tools/recomp/ghidra/ExportDecompAll.java (3.3.5): 64-bit addresses, names
// with their namespace. Drives tools/corpus.py --export.
// Decompiles every function in the program (or an address range) in parallel and streams the C
// output to a JSON-lines file, one object per function: {"addr","name","size","c"} or
// {"addr","name","size","error"}. This is the one-off export behind tools/recomp/corpus.py: once
// it has run, every later read of a reference function is a file lookup instead of a headless
// Ghidra run.
//
// Usage (headless): -postScript ExportDecompAll.java <outfile.jsonl> [<lo> <hi>]
//   lo/hi are hex addresses; hi is exclusive. Thunks are skipped (they carry no body worth reading).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.util.task.TaskMonitor;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class ExportDecompAll extends GhidraScript {

    private static String jsonEscape(String s) {
        StringBuilder b = new StringBuilder(s.length() + 16);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: ExportDecompAll <outfile.jsonl> [<lo> <hi>]");
            return;
        }
        long lo = 0, hi = Long.MAX_VALUE;
        if (args.length >= 3) {
            lo = Long.parseLong(args[1].replaceFirst("^0[xX]", ""), 16);
            hi = Long.parseLong(args[2].replaceFirst("^0[xX]", ""), 16);
        }
        // `@file`: only the functions at the addresses listed in it, one hex address a line (a
        // sample to check before a whole export)
        java.util.Set<Long> only = null;
        if (args.length == 2 && args[1].startsWith("@")) {
            only = new java.util.HashSet<>();
            for (String line : java.nio.file.Files.readAllLines(java.nio.file.Paths.get(args[1].substring(1)))) {
                if (!line.isBlank()) only.add(Long.parseLong(line.trim().replaceFirst("^0[xX]", ""), 16));
            }
        }

        List<Function> functions = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            if (f.isThunk()) {
                continue;
            }
            long a = f.getEntryPoint().getOffset();
            if (a < lo || a >= hi || (only != null && !only.contains(a))) {
                continue;
            }
            functions.add(f);
        }
        println("ExportDecompAll: " + functions.size() + " functions to decompile");

        DecompilerCallback<String[]> callback = new DecompilerCallback<String[]>(currentProgram,
                new DecompileConfigurer() {
                    @Override
                    public void configure(DecompInterface ifc) {
                        ifc.toggleCCode(true);
                        ifc.toggleSyntaxTree(true);
                        ifc.setSimplificationStyle("decompile");
                    }
                }) {
            @Override
            public String[] process(DecompileResults res, TaskMonitor mon) {
                Function f = res.getFunction();
                String addr = String.format("%x", f.getEntryPoint().getOffset());
                String name = f.getName(true);  // with namespace: Lua::C_Container::GetBagName
                String size = Long.toString(f.getBody().getNumAddresses());
                if (res.decompileCompleted()) {
                    return new String[] { addr, name, size, res.getDecompiledFunction().getC(), null };
                }
                return new String[] { addr, name, size, null, res.getErrorMessage() };
            }
        };
        callback.setTimeout(90);
        // what failed in the first pass gets a second, alone, with ten times the time and the
        // decompiler's limits raised: the 90 s timeout lost five functions, among them the 833-case
        // FUN_140727d90 with its 268 KB frame (2026-10-16)
        DecompilerCallback<String[]> retry = new DecompilerCallback<String[]>(currentProgram,
                new DecompileConfigurer() {
                    @Override
                    public void configure(DecompInterface ifc) {
                        ifc.toggleCCode(true);
                        ifc.toggleSyntaxTree(true);
                        ifc.setSimplificationStyle("decompile");
                        DecompileOptions opts = new DecompileOptions();
                        opts.grabFromProgram(currentProgram);
                        opts.setMaxInstructions(400000);
                        opts.setMaxPayloadMBytes(400);
                        ifc.setOptions(opts);
                    }
                }) {
            @Override
            public String[] process(DecompileResults res, TaskMonitor mon) throws Exception {
                return callback.process(res, mon);
            }
        };
        retry.setTimeout(900);
        final List<Function> failed = new ArrayList<>();

        final int[] count = { 0 };
        final long start = System.currentTimeMillis();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            ParallelDecompiler.decompileFunctions(callback, currentProgram, functions.iterator(), r -> {
                if (r == null) {
                    return;
                }
                if (r[3] == null) {
                    synchronized (failed) {
                        failed.add(getFunctionAt(toAddr(Long.parseUnsignedLong(r[0], 16))));
                    }
                    return;
                }
                synchronized (out) {
                    StringBuilder b = new StringBuilder();
                    b.append("{\"addr\":\"").append(r[0]).append("\",\"name\":\"").append(jsonEscape(r[1]))
                     .append("\",\"size\":").append(r[2]);
                    if (r[3] != null) {
                        b.append(",\"c\":\"").append(jsonEscape(r[3])).append("\"}");
                    } else {
                        b.append(",\"error\":\"").append(jsonEscape(r[4] == null ? "" : r[4])).append("\"}");
                    }
                    out.println(b);
                    count[0]++;
                    if (count[0] % 1000 == 0) {
                        long s = (System.currentTimeMillis() - start) / 1000;
                        println("ExportDecompAll: " + count[0] + " done, " + s + "s");
                        out.flush();
                    }
                }
            }, monitor);
            println("ExportDecompAll: " + failed.size() + " failed, decompiling them again with longer limits");
            failed.removeIf(f -> f == null);
            ParallelDecompiler.decompileFunctions(retry, currentProgram, failed.iterator(), r -> {
                if (r == null) {
                    return;
                }
                synchronized (out) {
                    StringBuilder b = new StringBuilder();
                    b.append("{\"addr\":\"").append(r[0]).append("\",\"name\":\"").append(jsonEscape(r[1]))
                     .append("\",\"size\":").append(r[2]);
                    if (r[3] != null) {
                        b.append(",\"c\":\"").append(jsonEscape(r[3])).append("\"}");
                    } else {
                        b.append(",\"error\":\"").append(jsonEscape(r[4] == null ? "" : r[4])).append("\"}");
                    }
                    out.println(b);
                    count[0]++;
                    println("ExportDecompAll: again " + r[0] + (r[3] != null ? " decompiled" : " failed: "
                            + (r[4] == null ? "" : r[4].trim().split("\n")[0])));
                }
            }, monitor);
        } finally {
            callback.dispose();
            retry.dispose();
        }
        long s = (System.currentTimeMillis() - start) / 1000;
        println("ExportDecompAll: wrote " + count[0] + " functions to " + args[0] + " in " + s + "s");
    }
}
