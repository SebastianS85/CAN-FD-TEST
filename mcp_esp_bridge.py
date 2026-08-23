import os
import sys
import json
import base64
import hashlib
import requests
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer

# ==========================================
# CONFIGURATION
# ==========================================
AUTHORIZATION_ENDPOINT = "https://mcp.espressif.com/docs/authorize"
TOKEN_ENDPOINT = "https://mcp.espressif.com/docs/token"
REGISTRATION_ENDPOINT = "https://mcp.espressif.com/docs/register"
MCP_API_ENDPOINT = "https://mcp.espressif.com/docs"

PORT = 8080
REDIRECT_URI = f"http://localhost:{PORT}"
auth_code = None

# Token caching to prevent opening the browser on every VS Code startup
TOKEN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".esp_mcp_token.json")

# ==========================================
# HELPER: LOGGING TO STDERR
# ==========================================
def log(message):
    """
    All logs MUST go to stderr. Printing to stdout will break the MCP protocol 
    because Continue expects only pure JSON on stdout.
    """
    sys.stderr.write(f"[Espressif Bridge] {message}\n")
    sys.stderr.flush()

# ==========================================
# AUTHENTICATION
# ==========================================
class OAuthCallbackHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global auth_code
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if 'code' in query:
            auth_code = query['code'][0]
            self.send_response(200)
            self.send_header('Content-type', 'text/html; charset=utf-8')
            self.end_headers()
            html = "<html><body style='text-align:center; padding-top:50px;'><h2 style='color:#4CAF50;'>MCP Bridge Authorized!</h2><p>You can close this tab and return to VS Code.</p></body></html>"
            self.wfile.write(html.encode('utf-8'))
        else:
            self.send_response(400)
            self.end_headers()

    def log_message(self, format, *args):
        pass # Suppress HTTP logs

def generate_pkce_pair():
    verifier = base64.urlsafe_b64encode(os.urandom(32)).decode('utf-8').rstrip('=')
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode('utf-8')).digest()).decode('utf-8').rstrip('=')
    return verifier, challenge

def get_access_token():
    # 1. Try to load cached token
    if os.path.exists(TOKEN_FILE):
        try:
            with open(TOKEN_FILE, 'r') as f:
                data = json.load(f)
                if "access_token" in data:
                    log("Loaded cached token. (Delete .esp_mcp_token.json if you get 401 Unauthorized errors)")
                    return data["access_token"]
        except Exception as e:
            log(f"Failed to read cache: {e}")

    # 2. If no token, perform OAuth flow
    log("No token found. Starting OAuth flow...")
    reg_response = requests.post(REGISTRATION_ENDPOINT, json={
        "redirect_uris": [REDIRECT_URI],
        "token_endpoint_auth_method": "none"
    })
    
    if reg_response.status_code not in (200, 201):
        log(f"Registration failed: {reg_response.text}")
        sys.exit(1)
        
    client_id = reg_response.json().get("client_id")
    code_verifier, code_challenge = generate_pkce_pair()

    server = HTTPServer(('localhost', PORT), OAuthCallbackHandler)
    
    auth_params = {
        "client_id": client_id,
        "redirect_uri": REDIRECT_URI,
        "response_type": "code",
        "scope": "read:user",
        "code_challenge": code_challenge,
        "code_challenge_method": "S256"
    }
    
    auth_url = f"{AUTHORIZATION_ENDPOINT}?{urllib.parse.urlencode(auth_params)}"
    log("Opening browser for authorization...")
    webbrowser.open(auth_url)
    
    server.handle_request() 
    
    if not auth_code:
        log("Authorization failed.")
        sys.exit(1)
        
    token_response = requests.post(TOKEN_ENDPOINT, data={
        "client_id": client_id,
        "grant_type": "authorization_code",
        "code": auth_code,
        "redirect_uri": REDIRECT_URI,
        "code_verifier": code_verifier
    })
    
    if token_response.status_code == 200:
        token = token_response.json().get("access_token")
        # Save token to file
        with open(TOKEN_FILE, 'w') as f:
            json.dump({"access_token": token}, f)
        log("Token obtained and cached successfully.")
        return token
    else:
        log(f"Failed to get token: {token_response.text}")
        sys.exit(1)

# ==========================================
# MCP STDIO PROXY SERVER
# ==========================================
def run_mcp_proxy(token):
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
        "Accept": "application/json"
    }
    
    log("Starting stdio proxy loop. Waiting for Continue requests...")
    
    # Read from standard input indefinitely (Continue sends requests here)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
            
        try:
            # We don't even need to parse the inner JSON deeply. 
            # We just forward the raw JSON-RPC payload to Espressif.
            payload = json.loads(line)
            
            # Send to Espressif cloud
            response = requests.post(MCP_API_ENDPOINT, headers=headers, json=payload)
            
            # Forward the response back to Continue via stdout
            # It MUST be a single line of valid JSON
            sys.stdout.write(response.text + "\n")
            sys.stdout.flush()
            
        except json.JSONDecodeError:
            log("Received invalid JSON from stdin")
        except Exception as e:
            # Continue will handle the lack of response, we just log to stderr
            log(f"Error forwarding request: {e}")

if __name__ == "__main__":
    access_token = get_access_token()
    if access_token:
        run_mcp_proxy(access_token)