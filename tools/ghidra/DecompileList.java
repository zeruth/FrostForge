// Decompiles a list of functions and writes the C output to a file.
// Usage (headless): -postScript DecompileList.java <outfile> <addr1> <addr2> ...
// Addresses are hex (with or without 0x). An address inside a function resolves to that function.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DecompileList <outfile> <addr>...");
            return;
        }
        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(true);
        ifc.toggleSyntaxTree(true);
        ifc.setSimplificationStyle("decompile");
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                String s = args[i].toLowerCase().startsWith("0x") ? args[i].substring(2) : args[i];
                Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(Long.parseLong(s, 16));
                Function f = getFunctionContaining(addr);
                if (f == null) {
                    out.println("// " + args[i] + ": no function here");
                    continue;
                }
                DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint() + " (requested " + args[i] + ") =====");
                if (res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("// decompile failed: " + res.getErrorMessage());
                }
                out.println();
            }
        }
        ifc.dispose();
    }
}
