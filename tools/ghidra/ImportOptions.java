// Pre-analysis options for importing WowClassic-clean.exe.
//
// "Non-Returning Functions - Discovered" is off: on this binary it spent 30+ minutes in
// ClearFlowAndRepairCmd re-disassembling after every call it decided does not return,
// without moving on (2026-10-08). .pdata already gives exact extents for all 74.8k
// functions, which is most of what that pass is for; the genuine no-return functions
// (assert / fatal handlers) are marked afterwards from the source-path anchors.
//
// Usage (headless): -preScript ImportOptions.java
//@category FrostLite
import ghidra.app.script.GhidraScript;
import java.util.Map;

public class ImportOptions extends GhidraScript {
    private static final String[][] OPTIONS = {
        {"Non-Returning Functions - Discovered", "false"},
    };

    @Override
    public void run() throws Exception {
        Map<String, String> current = getCurrentAnalysisOptionsAndValues(currentProgram);
        for (String[] o : OPTIONS) {
            if (!current.containsKey(o[0])) {
                printerr("ImportOptions: no analyzer option named '" + o[0] + "'");
                continue;
            }
            setAnalysisOption(currentProgram, o[0], o[1]);
            println("ImportOptions: " + o[0] + " = " + o[1]);
        }
    }
}
