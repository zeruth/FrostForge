// For each address: the instruction before it, the function's body ranges around it, and what
// disassembling there does (the command's own status message). A look at why Ghidra left code
// undefined, before changing ApplyFacts' disassembly steps.
//   bash tools/ghidra/ghidra.sh script ProbeDisasm.java 0x1418f6e6d
// @category FrostLite
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class ProbeDisasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String s : getScriptArgs()) {
            Address at = toAddr(Long.parseUnsignedLong(s.replace("0x", ""), 16));
            Instruction before = getInstructionBefore(at);
            println("PROBE " + s + ": before " + (before == null ? "none" : before.getMinAddress() + " " + before
                    + " flow " + before.getFlowType() + " override " + before.getFlowOverride()
                    + " fallthrough " + before.getFallThrough()));
            Function f = getFunctionContaining(before == null ? at : before.getMinAddress());
            if (f != null) {
                StringBuilder r = new StringBuilder();
                for (AddressRange ar : f.getBody()) r.append(ar.getMinAddress()).append("..").append(ar.getMaxAddress()).append(" ");
                println("PROBE " + s + ": " + f.getName() + " body " + r);
            }
            println("PROBE " + s + ": data " + currentProgram.getListing().getDataAt(at)
                    + " undefined " + (currentProgram.getListing().getUndefinedDataAt(at) != null));
            DisassembleCommand cmd = new DisassembleCommand(at, null, true);
            boolean ok = cmd.applyTo(currentProgram, monitor);
            Instruction now = getInstructionAt(at);
            println("PROBE " + s + ": disassemble " + ok + " status '" + cmd.getStatusMsg() + "' now "
                    + (now == null ? "none" : now.toString()) + " set " + cmd.getDisassembledAddressSet().getNumAddresses());
        }
    }
}
