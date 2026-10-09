// Removes functions that sit on a chained .pdata piece rather than a real entry point.
//
// About half the .pdata entries are out-of-line pieces of a larger function
// (UNW_FLAG_CHAININFO). An early ApplyFacts run created functions at some piece starts,
// which splits the parent in two. This lists, and with "remove" deletes, every function
// whose entry is a piece start, then lets the parent's body re-absorb the piece.
// Only DEFAULT-named (FUN_) functions are removed; analysis- or user-named ones are kept.
//
// Usage (headless): -postScript RemovePieceFunctions.java <pieces.json> [remove]
//   pieces.json: {"pieces": ["0x...", ...]}  (written by tools/pieces.py)
//@category FrostLite
import com.google.gson.JsonElement;
import com.google.gson.JsonParser;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.File;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

public class RemovePieceFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean remove = args.length > 1 && args[1].equals("remove");
        var root = JsonParser.parseString(Files.readString(new File(args[0]).toPath())).getAsJsonObject();
        Map<String, Integer> bySource = new HashMap<>();
        Set<Address> parents = new TreeSet<>();
        int removed = 0, kept = 0, thunksRemoved = 0;
        java.util.List<String> failed = new java.util.ArrayList<>();
        for (JsonElement el : root.getAsJsonArray("pieces")) {
            Address a = toAddr(Long.parseUnsignedLong(el.getAsString().substring(2), 16));
            Function f = getFunctionAt(a);
            if (f == null) continue;
            SourceType src = f.getSymbol().getSource();
            bySource.merge(src.toString(), 1, Integer::sum);
            // only DEFAULT (FUN_...) functions: those are the ones the early ApplyFacts run
            // created. Analysis- or user-named functions on a piece are left for review.
            if (src != SourceType.DEFAULT) {
                kept++;
                continue;
            }
            if (remove) {
                // Ghidra's FunctionManagerDB.doRemoveFunction can throw a
                // ConcurrentModificationException (seen in 12.1.3) while it walks a
                // HashMap it is editing. It is not about thunks (none were involved); it
                // depends on state built up within one run, and the same function removes
                // cleanly in a fresh run. So: log the address, carry on, and rerun until
                // the dry run reports no DEFAULT functions left (2026-10-08: three passes,
                // 1,727 + 47 + 1, plus 12 from an aborted first try).
                try {
                    Address[] thunks = f.getFunctionThunkAddresses(true);
                    if (thunks != null) {
                        for (Address t : thunks) {
                            Function tf = getFunctionAt(t);
                            if (tf != null && tf.getSymbol().getSource() == SourceType.DEFAULT) {
                                removeFunction(tf);
                                thunksRemoved++;
                            }
                        }
                    }
                    removeFunction(f);
                    removed++;
                    Function parent = getFunctionBefore(a);
                    if (parent != null) parents.add(parent.getEntryPoint());
                } catch (java.util.ConcurrentModificationException ex) {
                    failed.add(a.toString());
                }
            }
        }
        if (!failed.isEmpty()) {
            println("RemovePieceFunctions: " + failed.size() + " removals threw, first: "
                    + failed.subList(0, Math.min(10, failed.size())));
        }
        // let each affected parent re-flow into the freed piece
        for (Address p : parents) {
            CreateFunctionCmd.fixupFunctionBody(currentProgram, getFunctionAt(p), monitor);
        }
        println("RemovePieceFunctions: functions on piece starts by name source " + bySource
                + (remove ? "; removed " + removed + ", kept " + kept + " non-default, re-flowed "
                + parents.size() + " parents, " + thunksRemoved + " thunks removed first" : " (dry run)"));
    }
}
