# tests/trex/nat_scale_astf.py
# High-scale stateful TRex ASTF profile modeling 10^6 concurrent sessions with 15% churn.

from trex.astf.api import *

class NatScaleProfile:
    def create_profile(self):
        http_req = b"GET /1k.bin HTTP/1.1\r\nHost: benchmark.local\r\nConnection: close\r\n\r\n"
        http_res = b"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n" + (b"X" * 1024)

        prog = ASTFProgram()
        prog.send(http_req)
        prog.recv(len(http_res))

        assoc = ASTFAssociation(rules=ASTFAssociationRule(port=80))

        # 64,000 active client IPs opening multiple sessions to hit 10^6 total
        ip_gen_c = ASTFIpGenDist(ip_range=["10.10.0.2", "10.10.250.254"], distribution="rand")
        ip_gen_s = ASTFIpGenDist(ip_range=["198.18.1.1", "198.18.10.254"], distribution="rand")
        ip_gen = ASTFIpGen(glob=ASTFIpGenGlobal(ip_offset="1.0.0.0"),
                           dist_client=ip_gen_c,
                           dist_server=ip_gen_s)

        # 85% steady long-lived flows, 15% churn flows
        template_long = ASTFTemplate(program=prog, ip_gen=ip_gen, limit=850000)
        template_churn = ASTFTemplate(program=prog, ip_gen=ip_gen, limit=150000)

        cap = ASTFCapInfo(file="http_churn", cps=150000, assoc=assoc)

        profile = ASTFProfile(
            default_ip_gen=ip_gen,
            templates=[template_long, template_churn],
            cap_list=[cap]
        )
        return profile

def register():
    return NatScaleProfile()
