// The references into [lo, hi] from outside it, with what makes each (a look at why a dead region
// is still reached).
//   bash tools/ghidra/ghidra.sh script ProbeRefsInto.java 0x1424dbf47 0x1424dc3b6
// @category FrostLite
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class ProbeRefsInto extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Address lo = toAddr(Long.parseUnsignedLong(a[0].replace("0x", ""), 16));
        Address hi = toAddr(Long.parseUnsignedLong(a[1].replace("0x", ""), 16));
        ReferenceManager rm = currentProgram.getReferenceManager();
        AddressIterator it = rm.getReferenceDestinationIterator(new AddressSet(lo, hi), true);
        while (it.hasNext()) {
            Address d = it.next();
            for (Reference r : rm.getReferencesTo(d)) {
                Address f = r.getFromAddress();
                boolean outside = f.compareTo(lo) < 0 || f.compareTo(hi) > 0;
                println("PROBE " + d + " <- " + f + " " + r.getReferenceType() + (outside ? " OUTSIDE " : " inside ")
                        + getInstructionAt(f));
            }
        }
    }
}
