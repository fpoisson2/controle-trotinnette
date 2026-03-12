FROM node:20-alpine
WORKDIR /app
COPY package.json .
RUN npm install
COPY proxy.js .
COPY index.html .
CMD ["node", "proxy.js"]
