//@category Custom
import ghidra.app.script.GhidraScript;
import ghidra.app.plugin.core.analysis.PdbUniversalAnalyzer;
import java.io.File;
public class SetPdb extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PdbUniversalAnalyzer.setPdbFileOption(currentProgram, new File(a[0]));
        println("pdb set: " + a[0]);
    }
}
