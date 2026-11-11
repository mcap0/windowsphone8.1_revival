//@category Custom
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpAll extends GhidraScript {
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        PrintWriter syms = new PrintWriter(new FileWriter(a[0] + ".funcs"));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            syms.println(f.getEntryPoint() + " " + f.getName(true));
            DecompileResults r = di.decompileFunction(f, 90, monitor);
            out.println("//==== " + f.getEntryPoint() + " " + f.getName(true));
            if (r != null && r.decompileCompleted()) out.println(r.getDecompiledFunction().getC());
            else out.println("// decompile failed: " + (r == null ? "" : r.getErrorMessage()));
        }
        out.close(); syms.close();
    }
}
