package dumbphone.ponte;

import android.net.IpConfiguration;
import android.net.LinkAddress;
import android.net.StaticIpConfiguration;
import android.net.wifi.WifiConfiguration;

import java.lang.reflect.Method;
import java.net.InetAddress;

// One-shot, as the shell user (NETWORK_SETTINGS), once per phone: Wi-Fi network <ssid> gets a fixed address.
//   CLASSPATH=agent.dex app_process / dumbphone.ponte.StaticIp Capo-fibra 192.168.1.112/24 192.168.1.1 192.168.1.1
//   ... StaticIp Capo-fibra show | ... StaticIp Capo-fibra dhcp
public final class StaticIp {
    private static Object wifi;

    // Straight to the service: WifiManager would send the system context's package ("android"), refused for shell.
    private static Object call(String name, Object... first) throws Exception {
        for (Method m : wifi.getClass().getMethods()) {
            if (!m.getName().equals(name)) continue;
            Class<?>[] t = m.getParameterTypes();
            Object[] args = new Object[t.length];
            int i = 0;
            for (Object f : first) args[i++] = f;
            if (i < t.length && t[i] == String.class) args[i++] = Sys.PKG;
            if (i < t.length && t[i] == String.class) args[i++] = null;    // feature id
            for (; i < t.length; i++) if (t[i] == android.os.Bundle.class) args[i] = extras();
            return m.invoke(wifi, args);
        }
        throw new Exception("manca " + name);
    }

    private static android.os.Bundle extras() throws Exception {
        android.os.Bundle b = new android.os.Bundle();
        String key = (String) android.net.wifi.WifiManager.class.getField("EXTRA_PARAM_KEY_ATTRIBUTION_SOURCE").get(null);
        b.putParcelable(key, Sys.context().getAttributionSource());
        return b;
    }

    private static WifiConfiguration find(String ssid) throws Exception {
        Object slice = call("getPrivilegedConfiguredNetworks");
        for (Object o : (java.util.List<?>) slice.getClass().getMethod("getList").invoke(slice)) {
            WifiConfiguration c = (WifiConfiguration) o;
            if (c.SSID.equals("\"" + ssid + "\"")) return c;
        }
        return null;
    }

    public static void main(String[] a) {
        android.os.Looper.prepareMainLooper();
        try {
            run(a);
        } catch (Throwable t) {
            t.printStackTrace();
        }
        System.exit(0);
    }

    private static void run(String[] a) throws Exception {
        Class<?> sm = Class.forName("android.os.ServiceManager");
        Object binder = sm.getMethod("getService", String.class).invoke(null, "wifi");
        wifi = Class.forName("android.net.wifi.IWifiManager$Stub").getMethod("asInterface", android.os.IBinder.class).invoke(null, binder);
        WifiConfiguration cfg = find(a[0]);
        if (cfg == null) throw new Exception("rete non trovata: " + a[0]);
        Method get = WifiConfiguration.class.getMethod("getIpConfiguration");
        System.out.println("prima: " + get.invoke(cfg));
        if (a[1].equals("show")) return;
        Method set = WifiConfiguration.class.getMethod("setIpConfiguration", IpConfiguration.class);
        if (a[1].equals("dhcp")) {
            set.invoke(cfg, new IpConfiguration.Builder().build());
        } else {
            StaticIpConfiguration st = new StaticIpConfiguration.Builder()
                    .setIpAddress(LinkAddress.class.getConstructor(String.class).newInstance(a[1]))
                    .setGateway(InetAddress.getByName(a[2]))
                    .setDnsServers(java.util.List.of(InetAddress.getByName(a[3]))).build();
            set.invoke(cfg, new IpConfiguration.Builder().setStaticIpConfiguration(st).build());
        }
        System.out.println("addOrUpdateNetwork -> " + call("addOrUpdateNetwork", cfg));
        System.out.println("dopo: " + get.invoke(find(a[0])));
    }

    private StaticIp() {}
}
