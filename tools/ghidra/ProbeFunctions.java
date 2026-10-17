// Prints what Ghidra holds for the functions at the given addresses: entry, body extent,
// inline / thunk / noreturn flags, calling convention, signature source, and which function
// contains each address. A quick look before changing ApplyFacts.
//   bash tools/ghidra/ghidra.sh script ProbeFunctions.java 0x141832146 0x1418a72a0
// @category FrostLite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class ProbeFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address at = toAddr(Long.parseUnsignedLong(a.replace("0x", ""), 16));
            Function at_ = getFunctionAt(at);
            Function in = getFunctionContaining(at);
            Instruction insn = getInstructionAt(at);
            ghidra.program.model.listing.CodeUnit cu = currentProgram.getListing().getCodeUnitContaining(at);
            if (in != null && insn != null && insn.getFlowType().isCall()) {
                Class<?> util = Function.class.getClassLoader().loadClass("ghidra.program.model.pcode.HighFunctionDBUtil");
                for (java.lang.reflect.Method m : util.getMethods()) {
                    if (m.getName().toLowerCase().contains("override")) println("PROBE method " + m);
                }
                // the override symbols live in the function's namespace, named by the call's address
                for (ghidra.program.model.symbol.Symbol s : currentProgram.getSymbolTable().getSymbols(at)) {
                    Object o = util.getMethod("readOverride", ghidra.program.model.symbol.Symbol.class).invoke(null, s);
                    println("PROBE " + a + " symbol " + s.getName(true) + " " + s.getSymbolType() + " -> " + o);
                }
            }
            println("PROBE " + a + ": function at " + describe(at_) + "; containing " + describe(in)
                    + "; instruction " + (insn == null ? "none" : insn + " flow " + insn.getFlowOverride()
                    + " type " + insn.getFlowType())
                    + "; code unit " + (cu == null ? "none" : cu.getClass().getSimpleName() + " " + cu.getMinAddress()
                    + ".." + cu.getMaxAddress() + " " + cu));
        }
    }

    private String describe(Function f) {
        if (f == null) return "none";
        return f.getName() + " @" + f.getEntryPoint() + " body " + f.getBody().getMinAddress() + ".."
                + f.getBody().getMaxAddress() + " (" + f.getBody().getNumAddressRanges() + " ranges) inline="
                + f.isInline() + " thunk=" + f.isThunk() + " noreturn=" + f.hasNoReturn() + " cc="
                + f.getCallingConventionName() + " source=" + f.getSignatureSource();
    }
}
