Web upravljanje s autorizacijom mogu napraviti odmah, jer ne ovisi o `wifi_net.c`. Radim to sad.

Web upravljanje s autorizacijom je dodano, ali AP fallback još nije jer i dalje nemam `wifi_net.c` i `wifi_net.h`. Ništa od ovoga nisam mogao kompajlirati; provjerio sam samo JavaScript stranice i base64 kodiranje prijave na računalu.

**Lozinka za AP**

"mario23" ima 7 znakova, a WPA2 traži 8 do 63, pa je WiFi stack odbija i AP se ne bi podigao. U novom `web_cfg.h` privremeno piše `AP_PASSWORD "mario230"`. Promijeni je u što želiš, ali neka ima barem 8 znakova. SSID je `M5-SDR`, a adresa 192.168.4.1 je zadana za ESP-IDF AP.

**Autorizacija i upravljanje**
- Cijela stranica, prikaz i upravljanje traže prijavu (HTTP Basic). Korisnik i lozinka su u `web_cfg.h` (`WEB_USER` je "admin", `WEB_PASSWORD` je prazan). Te dvije vrijednosti postavi sam.
- Dok je `WEB_PASSWORD` prazan, stranica je samo za pregled, a upravljanje se odbija. Tako uređaj ne ostane otvoren ako zaboraviš lozinku.
- Klik i povlačenje mišem ili prstom po slici šalju se kao dodir na ekran. To znači da radi sve što i dodir na uređaju: tipke, tipkovnica za frekvenciju, povlačenje spektra za tuniranje i tap na spektar.
- Zumiranje s dva prsta preko weba nije moguće.
- Ako dodirneš stvarni ekran, on ima prednost nad webom. Ako web klijent nestane dok drži pritisak, pritisak se otpusti nakon 2 s.
- Ekrani postavki i dalje se ne prenose na web, a na njima se ni daljinsko upravljanje ne prihvaća, da se lozinke ne vide ni ne upisuju naslijepo.
- Zahtjev za dodir traži i posebno zaglavlje `X-M5`. Preglednik ga ne dodaje zahtjevima s tuđih stranica, pa druga web stranica ne može upravljati radiom tvojom spremljenom prijavom.

**Ograničenja**
- Basic prijava nije šifrirana. Na kućnoj mreži netko tko prisluškuje promet može vidjeti lozinku, pa je nemoj ponavljati drugdje. Na AP-u je veza šifrirana WPA2. Šifriranje na kućnoj mreži zahtijevalo bi HTTPS i certifikat, što možemo dodati ako želiš.
- HTTP server obrađuje jedan zahtjev po jedan, pa dodir zna kasniti dok se šalje slika. Kvačica "low bandwidth" to ublažava.

**Za AP fallback pošalji** `wifi_net.c` i `wifi_net.h`. Kad ih dobijem, dodajem da se AP "M5-SDR" digne ako se STA ne spoji (ili nema podataka), uz pregled na 192.168.4.1.