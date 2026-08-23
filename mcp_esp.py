import os
import base64
import hashlib
import requests
import urllib.parse
import webbrowser
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

# ==========================================
# ESPRESSIF METADATA CONFIGURATION
# ==========================================
# Endpoint URLs fetched from Espressif's .well-known configuration
AUTHORIZATION_ENDPOINT = "https://mcp.espressif.com/docs/authorize"
TOKEN_ENDPOINT = "https://mcp.espressif.com/docs/token"
REGISTRATION_ENDPOINT = "https://mcp.espressif.com/docs/register"

# Target endpoint where the Model Context Protocol (MCP) operates
MCP_API_ENDPOINT = "https://mcp.espressif.com/docs" 

# Local server settings to intercept the GitHub authorization code
PORT = 8080
REDIRECT_URI = f"http://localhost:{PORT}"
auth_code = None  # Global variable storing the intercepted authorization code

# ==========================================
# LOCAL CALLBACK SERVER
# ==========================================
class OAuthCallbackHandler(BaseHTTPRequestHandler):
    """
    Simple HTTP server listening on port 8080.
    GitHub will redirect the browser here after a successful login,
    appending the ?code=... parameter to the URL.
    """
    def do_GET(self):
        global auth_code
        # Parse the URL to extract parameters after the question mark
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        
        # If GitHub returned the 'code' parameter, save it
        if 'code' in query:
            auth_code = query['code'][0]
            
            # Send HTTP 200 (OK) response to the browser
            self.send_response(200)
            self.send_header('Content-type', 'text/html; charset=utf-8')
            self.end_headers()
            
            # Simple HTML page indicating success
            html = """
            <html>
                <body style="font-family: sans-serif; text-align: center; padding-top: 50px;">
                    <h2 style="color: #4CAF50;">Authentication successful!</h2>
                    <p>Python script received the authorization code. You can close this tab.</p>
                </body>
            </html>
            """
            self.wfile.write(html.encode('utf-8'))
        else:
            # If the 'code' parameter is missing, return a 400 (Bad Request) error
            self.send_response(400)
            self.end_headers()

    def log_message(self, format, *args):
        # Overriding this method hides the standard server logs in the console
        pass

# ==========================================
# PKCE MECHANISM AND AUTHORIZATION
# ==========================================
def generate_pkce_pair():
    """
    Generates a cryptographic pair (verifier and challenge) for the PKCE mechanism.
    This allows for a secure login without the need for a static client_secret.
    """
    # Random string (verifier)
    verifier = base64.urlsafe_b64encode(os.urandom(32)).decode('utf-8').rstrip('=')
    
    # SHA256 hash of the verifier (challenge), sent with the initial request
    challenge = base64.urlsafe_b64encode(
        hashlib.sha256(verifier.encode('utf-8')).digest()
    ).decode('utf-8').rstrip('=')
    
    return verifier, challenge

def run_espressif_auth():
    """
    Main function conducting the full OAuth2 + PKCE login flow.
    """
    print("-> 1. Dynamic client registration...")
    # Registering our script with the server to get a temporary client_id
    reg_response = requests.post(REGISTRATION_ENDPOINT, json={
        "redirect_uris": [REDIRECT_URI],
        "token_endpoint_auth_method": "none" # 'none' means a public client without a static password
    })
    
    if reg_response.status_code not in (200, 201):
        print(f"[ERROR] Registration failed: {reg_response.text}")
        return None
        
    client_id = reg_response.json().get("client_id")
    print(f"   Obtained temporary Client ID: {client_id}")

    code_verifier, code_challenge = generate_pkce_pair()

    print("-> 2. Opening browser for GitHub authorization...")
    # Start local server on port 8080
    server = HTTPServer(('localhost', PORT), OAuthCallbackHandler)
    
    # Build the login URL with required parameters
    auth_params = {
        "client_id": client_id,
        "redirect_uri": REDIRECT_URI,
        "response_type": "code",
        "scope": "read:user",
        "code_challenge": code_challenge,
        "code_challenge_method": "S256"
    }
    auth_url = f"{AUTHORIZATION_ENDPOINT}?{urllib.parse.urlencode(auth_params)}"
    
    # Open default web browser
    webbrowser.open(auth_url)
    
    print("-> 3. Waiting for browser response...")
    # Pause script and wait for exactly one request from the browser
    server.handle_request() 
    
    if not auth_code:
        print("[ERROR] Authorization code not intercepted.")
        return None
        
    print("-> 4. Exchanging authorization code for Access Token...")
    # Send the received code along with 'code_verifier' to prove our identity
    token_payload = {
        "client_id": client_id,
        "grant_type": "authorization_code",
        "code": auth_code,
        "redirect_uri": REDIRECT_URI,
        "code_verifier": code_verifier
    }
    
    token_response = requests.post(TOKEN_ENDPOINT, data=token_payload)
    
    if token_response.status_code == 200:
        access_token = token_response.json().get("access_token")
        print("\n=== SUCCESS ===")
        print(f"Obtained Access Token: Bearer {access_token[:15]}...\n")
        return access_token
    else:
        print(f"[ERROR] Failed to get token: {token_response.text}")
        return None

# ==========================================
# INTERACTIVE QUERY LOOP
# ==========================================
def interactive_chat(token):
    """
    Function maintaining a console loop, allowing the user to continuously 
    ask questions to the Espressif MCP server in English.
    """
    # Set headers required by the MCP server (token + data types)
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
        "Accept": "application/json"  # Required by the server (Fix for 406 Error)
    }
    
    print("==================================================")
    print(" ESPRESSIF MCP TERMINAL")
    print(" Type 'exit' or 'quit' to close the session.")
    print("==================================================")
    
    while True:
        # Get the query from the user
        user_query = input("\n[Your query]> ")
        
        # Exit the program
        if user_query.lower() in ['exit', 'quit']:
            print("Closing session...")
            break
            
        # Empty query - ignore and ask again
        if not user_query.strip():
            continue
            
        # Build payload compliant with JSON-RPC 2.0 (standard used by MCP)
        payload = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {
                "name": "search_espressif_sources", # Matches the official tool name
                "arguments": {
                    "query": user_query,
                    "language": "en"  # Force English documentation
                }
            }
        }
        
        print("Sending request to Espressif MCP...")
        try:
            # Send POST request to the MCP server
            response = requests.post(MCP_API_ENDPOINT, headers=headers, json=payload)
            response.raise_for_status()
            
            # Read and format the response into readable JSON
            data = response.json()
            print("\n=== MCP RESPONSE ===")
            print(json.dumps(data, indent=2))
            
        except requests.exceptions.RequestException as e:
            print(f"\n[HTTP ERROR]: {e}")
            if e.response is not None:
                print(f"Server details: {e.response.text}")
        except json.JSONDecodeError:
            print(f"\n[JSON ERROR]: Received invalid format from server.")

# ==========================================
# MAIN ENTRY POINT
# ==========================================
if __name__ == "__main__":
    # Start the login process
    access_token = run_espressif_auth()
    
    # If login was successful, proceed to the interactive chat
    if access_token:
        interactive_chat(access_token)