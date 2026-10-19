// The flows Ghidra records from each given instruction (a computed jump's cases, as its own
// table recovery or an override left them).
//   bash tools/ghidra/ghidra.sh script ProbeFlows.java 0x14173554a
// @category FrostLite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class ProbeFlows extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String s : getScriptArgs()) {
            Instruction insn = getInstructionAt(toAddr(Long.parseUnsignedLong(s.replace("0x", ""), 16)));
            if (insn == null) {
                println("PROBE " + s + ": no instruction");
                continue;
            }
            StringBuilder b = new StringBuilder();
            for (Address a : insn.getFlows()) b.append(a).append(' ');
            ghidra.program.model.symbol.Reference[] refs = insn.getReferencesFrom();
            StringBuilder r = new StringBuilder();
            for (ghidra.program.model.symbol.Reference ref : refs) r.append(ref.getToAddress()).append('/').append(ref.getReferenceType()).append(' ');
            StringBuilder to = new StringBuilder();
            for (ghidra.program.model.symbol.Reference ref : currentProgram.getReferenceManager().getReferencesTo(insn.getMinAddress())) {
                to.append(ref.getFromAddress()).append('/').append(ref.getReferenceType()).append(' ');
            }
            Instruction prev = getInstructionBefore(insn.getMinAddress());
            println("PROBE " + s + ": " + insn + " flows " + b + "| refs " + r + "| to " + to + "| prev "
                    + (prev == null ? "none" : prev.getMinAddress() + " " + prev + " ft " + prev.getFallThrough()));
        }
    }
}
